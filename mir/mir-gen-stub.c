/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.

   Stub for MIR generator machine dependent file.  It contains
   definitions used by MIR generator.  You can use this file for
   successful compilation of mir-gen.c.

   See HOW-TO-PORT-MIR.md document for the definitions description.
*/

enum {
  R0_HARD_REG,
  R1_HARD_REG,
  R2_HARD_REG,
  R3_HARD_REG,
  R4_HARD_REG,
  R5_HARD_REG,
  R6_HARD_REG,
  R7_HARD_REG,
  F0_HARD_REG,
  F1_HARD_REG,
  F2_HARD_REG,
  F3_HARD_REG,
  F4_HARD_REG,
  F5_HARD_REG,
  F6_HARD_REG,
  F7_HARD_REG
};

static const MIR_reg_t MAX_HARD_REG = F7_HARD_REG; /* max value for the previous regs */
static const MIR_reg_t FP_HARD_REG = R6_HARD_REG;  /* stack frame pointer according ABI */
static const MIR_reg_t SP_HARD_REG = R7_HARD_REG;  /* stack pointer according ABI */

const MIR_reg_t TEMP_INT_HARD_REG1 = R2_HARD_REG, TEMP_INT_HARD_REG2 = R3_HARD_REG;
const MIR_reg_t TEMP_FLOAT_HARD_REG1 = F2_HARD_REG, TEMP_FLOAT_HARD_REG2 = F3_HARD_REG;
const MIR_reg_t TEMP_DOUBLE_HARD_REG1 = F2_HARD_REG, TEMP_DOUBLE_HARD_REG2 = F3_HARD_REG;
const MIR_reg_t TEMP_LDOUBLE_HARD_REG1 = F2_HARD_REG;
const MIR_reg_t TEMP_LDOUBLE_HARD_REG2 = F3_HARD_REG;

static int target_locs_num (MIR_reg_t loc, MIR_type_t type) {
  return loc > MAX_HARD_REG && type == MIR_T_LD ? 2 : 1;
}

static inline int target_hard_reg_type_ok_p (MIR_reg_t hard_reg, MIR_type_t type) {
  assert (hard_reg <= MAX_HARD_REG);
  return (type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD ? hard_reg >= F0_HARD_REG
                                                                 : hard_reg < F0_HARD_REG);
}

static inline int target_fixed_hard_reg_p (MIR_reg_t hard_reg) {
  assert (hard_reg <= MAX_HARD_REG);
  return (hard_reg == FP_HARD_REG || hard_reg == SP_HARD_REG || hard_reg == TEMP_INT_HARD_REG1
          || hard_reg == TEMP_INT_HARD_REG2 || hard_reg == TEMP_FLOAT_HARD_REG1
          || hard_reg == TEMP_FLOAT_HARD_REG2 || hard_reg == TEMP_DOUBLE_HARD_REG1
          || hard_reg == TEMP_DOUBLE_HARD_REG2 || hard_reg == TEMP_LDOUBLE_HARD_REG1
          || hard_reg == TEMP_LDOUBLE_HARD_REG2);
}

static inline int target_call_used_hard_reg_p (MIR_reg_t hard_reg) {
  assert (hard_reg <= MAX_HARD_REG);
  return !((hard_reg >= R4_HARD_REG && hard_reg <= R5_HARD_REG)
           || (hard_reg >= F2_HARD_REG && hard_reg <= F7_HARD_REG));
}

static const int slots_offset = 176; /* It is used in this file but not in MIR generator */

static MIR_disp_t target_get_stack_slot_offset (MIR_context_t ctx, MIR_type_t type,
                                                MIR_reg_t slot) {
  /* slot is 0, 1, ... */
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  return -((MIR_disp_t) (slot + (type == MIR_T_LD ? 2 : 1)) * 8 + slots_offset);
}

static const MIR_insn_code_t target_io_dup_op_insn_codes[] = {MIR_INSN_BOUND};

static int target_valid_mem_offset_p (gen_ctx_t gen_ctx, MIR_type_t type, MIR_disp_t offset) {
  return TRUE;
}

static void target_machinize (MIR_context_t ctx) {}

static void target_make_prolog_epilog (MIR_context_t ctx, bitmap_t used_hard_regs,
                                       size_t stack_slots_num) {}

static void target_get_early_clobbered_hard_regs (MIR_insn_t insn, MIR_reg_t *hr1, MIR_reg_t *hr2) {
  *hr1 = *hr2 = MIR_NON_HARD_REG;
}

static int target_insn_ok_p (MIR_context_t ctx, MIR_insn_t insn) { return FALSE; }
static uint8_t *target_translate (MIR_context_t ctx, size_t *len) { return NULL; }
static void target_rebase (MIR_context_t ctx, uint8_t *base) {}
static void target_change_to_direct_calls (MIR_context_t ctx) {}

static void target_init (MIR_context_t ctx) {
  fprintf (stderr, "Your generator target dependent file is just a stub!\n");
  fprintf (stderr, "MIR generator can not use it -- good bye.\n");
  exit (1);
}
static void target_finish (MIR_context_t ctx) {}
