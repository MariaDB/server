/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include "mir-ppc64.h"

#include <limits.h>

/* We don't use TOC.  So r2 is not necessary for the generated code.  */
static void fancy_abort (int code) {
  if (!code) abort ();
}
#undef gen_assert
#define gen_assert(c) fancy_abort (c)

#define TARGET_EXPAND_ADDO
#define TARGET_EXPAND_ADDOS
#define TARGET_EXPAND_UADDO
#define TARGET_EXPAND_UADDOS
#define TARGET_EXPAND_MULO
#define TARGET_EXPAND_MULOS
#define TARGET_EXPAND_UMULO
#define TARGET_EXPAND_UMULOS

static const MIR_reg_t LINK_HARD_REG = LR_HARD_REG;

static inline MIR_reg_t target_nth_loc (MIR_reg_t loc, MIR_type_t type MIR_UNUSED, int n) {
  return loc + n;
}

static inline int target_call_used_hard_reg_p (MIR_reg_t hard_reg, MIR_type_t type MIR_UNUSED) {
  assert (hard_reg <= MAX_HARD_REG);
  return ((R0_HARD_REG <= hard_reg && hard_reg <= R13_HARD_REG)
          || (F0_HARD_REG <= hard_reg && hard_reg <= F13_HARD_REG));
}

static MIR_reg_t target_get_stack_slot_base_reg (gen_ctx_t gen_ctx MIR_UNUSED) {
  return FP_HARD_REG;
}

/* Stack layout (r1(sp) refers to the last reserved stack slot
   address) from higher address to lower address memory:

        +-> Back chain                                    BE                 LE
        |   Floating point register save area             optional        optional
        |   General register save area                    optional        optional
        |   VRSAVE save word (32-bits)                      0              NA
        |   Alignment padding (4 or 12 bytes)
        |   Vector register save area (quadword aligned)    we don't have it
        |   Local variable space                          optional        optional
        |   Parameter save area  (for callees)            (SP + 48)       (SP + 32) optional
        |   TOC save area                                 (SP + 40)       (SP + 24)
        |   link editor doubleword (we don't use it)      (SP + 32)          NA
        |   compiler doubleword    (we don't use it)      (SP + 24)          NA
        |   LR save area (used by callee)                 (SP + 16)       (SP + 16)
        |   CR save area                                  (SP + 8)        (SP + 8)
SP,R31->+-- Back chain                                    (SP + 0)        (SP + 0)
            Alloca area (after that new 48 or 32 bytes header should be created with new values)

Originally SP(r1) and FP (r31) are the same but r1 can be changed by alloca */

/* ppc64 has 3-ops insns */
static const MIR_insn_code_t target_io_dup_op_insn_codes[] = {MIR_INSN_BOUND};

static MIR_insn_code_t get_ext_code (MIR_type_t type) {
  switch (type) {
  case MIR_T_I8: return MIR_EXT8;
  case MIR_T_U8: return MIR_UEXT8;
  case MIR_T_I16: return MIR_EXT16;
  case MIR_T_U16: return MIR_UEXT16;
  case MIR_T_I32: return MIR_EXT32;
  case MIR_T_U32: return MIR_UEXT32;
  default: return MIR_INVALID_INSN;
  }
}

struct insn_pattern_info {
  int start, num;
};

typedef struct insn_pattern_info insn_pattern_info_t;
DEF_VARR (insn_pattern_info_t);

enum branch_type { BRCOND, JUMP, LADDR, BCTR };
struct label_ref {
  int abs_addr_p;
  enum branch_type branch_type;
  size_t label_val_disp;
  union {
    MIR_label_t label;
    void *jump_addr; /* absolute addr for BBV */
  } u;
};

typedef struct label_ref label_ref_t;
DEF_VARR (label_ref_t);

struct target_ctx {
  unsigned char alloca_p, block_arg_func_p, leaf_p, switch_p, laddr_p, short_bb_branch_p;
  size_t param_save_area_size;
  MIR_insn_t temp_jump;
  const char *temp_jump_replacement;
  VARR (int) * pattern_indexes;
  VARR (insn_pattern_info_t) * insn_pattern_info;
  VARR (uint8_t) * result_code;
  VARR (label_ref_t) * label_refs;
  VARR (uint64_t) * abs_address_locs;
  VARR (MIR_code_reloc_t) * relocs;
};

#define alloca_p gen_ctx->target_ctx->alloca_p
#define block_arg_func_p gen_ctx->target_ctx->block_arg_func_p
#define leaf_p gen_ctx->target_ctx->leaf_p
#define switch_p gen_ctx->target_ctx->switch_p
#define laddr_p gen_ctx->target_ctx->laddr_p
#define short_bb_branch_p gen_ctx->target_ctx->short_bb_branch_p
#define param_save_area_size gen_ctx->target_ctx->param_save_area_size
#define temp_jump gen_ctx->target_ctx->temp_jump
#define temp_jump_replacement gen_ctx->target_ctx->temp_jump_replacement
#define pattern_indexes gen_ctx->target_ctx->pattern_indexes
#define insn_pattern_info gen_ctx->target_ctx->insn_pattern_info
#define result_code gen_ctx->target_ctx->result_code
#define label_refs gen_ctx->target_ctx->label_refs
#define abs_address_locs gen_ctx->target_ctx->abs_address_locs
#define relocs gen_ctx->target_ctx->relocs

static void gen_mov (gen_ctx_t gen_ctx, MIR_insn_t anchor, MIR_insn_code_t code, MIR_op_t dst_op,
                     MIR_op_t src_op) {
  gen_add_insn_before (gen_ctx, anchor, MIR_new_insn (gen_ctx->ctx, code, dst_op, src_op));
}

static void mir_blk_mov (uint64_t *to, uint64_t *from, uint64_t nwords) {
  for (; nwords > 0; nwords--) *to++ = *from++;
}

static const char *BLK_MOV = "mir.blk_mov";
static const char *BLK_MOV_P = "mir.blk_mov.p";

static void gen_blk_mov (gen_ctx_t gen_ctx, MIR_insn_t anchor, size_t to_disp,
                         MIR_reg_t to_base_hard_reg, size_t from_disp, MIR_reg_t from_base_reg,
                         size_t qwords, int save_regs) {
  MIR_context_t ctx = gen_ctx->ctx;
  MIR_func_t func = curr_func_item->u.func;
  MIR_item_t proto_item, func_import_item;
  MIR_insn_t new_insn;
  MIR_op_t ops[5], freg_op, treg_op, treg_op2, treg_op3;

  treg_op = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
  if (qwords <= 16) {
    for (; qwords > 0; qwords--, to_disp += 8, from_disp += 8) {
      gen_mov (gen_ctx, anchor, MIR_MOV, treg_op,
               _MIR_new_var_mem_op (ctx, MIR_T_I64, from_disp, from_base_reg, MIR_NON_VAR, 1));
      gen_mov (gen_ctx, anchor, MIR_MOV,
               _MIR_new_var_mem_op (ctx, MIR_T_I64, to_disp, to_base_hard_reg, MIR_NON_VAR, 1),
               treg_op);
    }
    return;
  }
  treg_op2 = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
  treg_op3 = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
  /* Save arg regs: */
  if (save_regs > 0) gen_mov (gen_ctx, anchor, MIR_MOV, treg_op, _MIR_new_var_op (ctx, 3));
  if (save_regs > 1) gen_mov (gen_ctx, anchor, MIR_MOV, treg_op2, _MIR_new_var_op (ctx, 4));
  if (save_regs > 2) gen_mov (gen_ctx, anchor, MIR_MOV, treg_op3, _MIR_new_var_op (ctx, 5));
  /* call blk move: */
  proto_item = _MIR_builtin_proto (ctx, curr_func_item->module, BLK_MOV_P, 0, NULL, 3, MIR_T_I64,
                                   "to", MIR_T_I64, "from", MIR_T_I64, "nwords");
  func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, BLK_MOV, mir_blk_mov);
  freg_op = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
  new_insn = MIR_new_insn (ctx, MIR_MOV, freg_op, MIR_new_ref_op (ctx, func_import_item));
  gen_add_insn_before (gen_ctx, anchor, new_insn);
  gen_add_insn_before (gen_ctx, anchor,
                       MIR_new_insn (gen_ctx->ctx, MIR_ADD, _MIR_new_var_op (ctx, 3),
                                     _MIR_new_var_op (ctx, to_base_hard_reg),
                                     MIR_new_int_op (ctx, to_disp)));
  gen_add_insn_before (gen_ctx, anchor,
                       MIR_new_insn (gen_ctx->ctx, MIR_ADD, _MIR_new_var_op (ctx, 4),
                                     _MIR_new_var_op (ctx, from_base_reg),
                                     MIR_new_int_op (ctx, from_disp)));
  gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, 5), MIR_new_int_op (ctx, qwords));
  ops[0] = MIR_new_ref_op (ctx, proto_item);
  ops[1] = freg_op;
  ops[2] = _MIR_new_var_op (ctx, 3);
  ops[3] = _MIR_new_var_op (ctx, 4);
  ops[4] = _MIR_new_var_op (ctx, 5);
  new_insn = MIR_new_insn_arr (ctx, MIR_CALL, 5, ops);
  gen_add_insn_before (gen_ctx, anchor, new_insn);
  /* Restore arg regs: */
  if (save_regs > 0) gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, 3), treg_op);
  if (save_regs > 1) gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, 4), treg_op2);
  if (save_regs > 2) gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, 5), treg_op3);
}

static void machinize_call (gen_ctx_t gen_ctx, MIR_insn_t call_insn) {
  MIR_context_t ctx = gen_ctx->ctx;
  MIR_func_t func = curr_func_item->u.func;
  MIR_proto_t proto = call_insn->ops[0].u.ref->u.proto;
  int vararg_p = proto->vararg_p;
  size_t qwords, disp, nargs, nops = MIR_insn_nops (ctx, call_insn), start = proto->nres + 2;
  size_t mem_size = 0, n_iregs = 0, n_fregs = 0;
  MIR_type_t type, mem_type;
  MIR_op_mode_t mode;
  MIR_var_t *arg_vars = NULL;
  MIR_reg_t ret_reg;
  MIR_op_t arg_op, temp_op, arg_reg_op, ret_reg_op, mem_op;
  MIR_insn_code_t new_insn_code, ext_code;
  MIR_insn_t new_insn, ext_insn;

  if (call_insn->code == MIR_INLINE) call_insn->code = MIR_CALL;
  if (proto->args == NULL) {
    nargs = 0;
  } else {
    gen_assert (nops >= VARR_LENGTH (MIR_var_t, proto->args)
                && (vararg_p || nops - start == VARR_LENGTH (MIR_var_t, proto->args)));
    nargs = VARR_LENGTH (MIR_var_t, proto->args);
    arg_vars = VARR_ADDR (MIR_var_t, proto->args);
  }
  if (call_insn->ops[1].mode != MIR_OP_VAR) {
    // ??? to optimize (can be immediate operand for func call)
    temp_op = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
    new_insn = MIR_new_insn (ctx, MIR_MOV, temp_op, call_insn->ops[1]);
    call_insn->ops[1] = temp_op;
    gen_add_insn_before (gen_ctx, call_insn, new_insn);
  }
  for (size_t i = start; i < nops; i++) {
    arg_op = call_insn->ops[i];
    gen_assert (arg_op.mode == MIR_OP_VAR
                || (arg_op.mode == MIR_OP_VAR_MEM && MIR_all_blk_type_p (arg_op.u.mem.type)));
    if (i - start < nargs) {
      type = arg_vars[i - start].type;
    } else if (call_insn->ops[i].mode == MIR_OP_VAR_MEM) {
      type = arg_op.u.mem.type;
      gen_assert (MIR_all_blk_type_p (type));
    } else {
      mode = call_insn->ops[i].value_mode;  // ??? smaller ints
      gen_assert (mode == MIR_OP_INT || mode == MIR_OP_UINT || mode == MIR_OP_FLOAT
                  || mode == MIR_OP_DOUBLE || mode == MIR_OP_LDOUBLE);
      if (mode == MIR_OP_FLOAT)
        (*MIR_get_error_func (ctx)) (MIR_call_op_error,
                                     "passing float variadic arg (should be passed as double)");
      type = mode == MIR_OP_DOUBLE ? MIR_T_D : mode == MIR_OP_LDOUBLE ? MIR_T_LD : MIR_T_I64;
    }
    ext_insn = NULL;
    if ((ext_code = get_ext_code (type)) != MIR_INVALID_INSN) { /* extend arg if necessary */
      temp_op = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
      ext_insn = MIR_new_insn (ctx, ext_code, temp_op, arg_op);
      call_insn->ops[i] = arg_op = temp_op;
    }
    mem_type = type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD ? type : MIR_T_I64;  // ???
    if ((type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD) && n_fregs < 13) {
      /* put arguments to argument hard regs */
      if (ext_insn != NULL) gen_add_insn_before (gen_ctx, call_insn, ext_insn);
      arg_reg_op = _MIR_new_var_op (ctx, F1_HARD_REG + n_fregs);
      gen_mov (gen_ctx, call_insn,
               type == MIR_T_F   ? MIR_FMOV
               : type == MIR_T_D ? MIR_DMOV
                                 : MIR_LDMOV,  // ???
               arg_reg_op, arg_op);
      call_insn->ops[i] = arg_reg_op;
      if (vararg_p) {                                             // ??? dead insns
        if (n_iregs >= 8 || (type == MIR_T_LD && n_iregs == 7)) { /* store in memory too */
          mem_op = _MIR_new_var_mem_op (ctx, mem_type, mem_size + PPC64_STACK_HEADER_SIZE,
                                        SP_HARD_REG, MIR_NON_VAR, 1);
          gen_assert (n_fregs < 12);
          gen_mov (gen_ctx, call_insn, type == MIR_T_LD ? MIR_LDMOV : MIR_DMOV, mem_op, arg_reg_op);
        }
        if (n_iregs < 8) { /* load into gp reg too */
          mem_op = _MIR_new_var_mem_op (ctx, mem_type, -16, SP_HARD_REG, MIR_NON_VAR, 1);
          gen_mov (gen_ctx, call_insn, type == MIR_T_LD ? MIR_LDMOV : MIR_DMOV, mem_op, arg_reg_op);
          mem_type = mem_type == MIR_T_F ? MIR_T_I32 : MIR_T_I64;  // ???
          mem_op = _MIR_new_var_mem_op (ctx, mem_type, -16, SP_HARD_REG, MIR_NON_VAR, 1);
          arg_reg_op = _MIR_new_var_op (ctx, R3_HARD_REG + n_iregs);
          gen_mov (gen_ctx, call_insn, MIR_MOV, arg_reg_op, mem_op);
          call_insn->ops[i] = arg_reg_op; /* keep it alive */
          if (type == MIR_T_LD && n_iregs + 1 < 8) {
            mem_op = _MIR_new_var_mem_op (ctx, mem_type, -8, SP_HARD_REG, MIR_NON_VAR, 1);
            gen_mov (gen_ctx, call_insn, MIR_MOV, _MIR_new_var_op (ctx, R3_HARD_REG + n_iregs + 1),
                     mem_op);
          }
        }
      }
      n_fregs += type == MIR_T_LD ? 2 : 1;
    } else if (MIR_blk_type_p (type)) {
      gen_assert (arg_op.mode == MIR_OP_VAR_MEM && arg_op.u.mem.disp >= 0
                  && arg_op.u.mem.index == MIR_NON_VAR);
      qwords = (arg_op.u.mem.disp + 7) / 8;
      for (disp = 0; qwords > 0 && n_iregs < 8; qwords--, n_iregs++, mem_size += 8, disp += 8) {
        arg_reg_op = _MIR_new_var_op (ctx, R3_HARD_REG + n_iregs);
        gen_mov (gen_ctx, call_insn, MIR_MOV, arg_reg_op,
                 _MIR_new_var_mem_op (ctx, MIR_T_I64, disp, arg_op.u.mem.base, MIR_NON_VAR, 1));
        setup_call_hard_reg_args (gen_ctx, call_insn, R3_HARD_REG + n_iregs);
      }
      if (qwords > 0)
        gen_blk_mov (gen_ctx, call_insn, mem_size + PPC64_STACK_HEADER_SIZE, SP_HARD_REG, disp,
                     arg_op.u.mem.base, qwords, n_iregs);
      mem_size += qwords * 8;
      n_iregs += qwords;
      continue;
    } else if (type != MIR_T_F && type != MIR_T_D && type != MIR_T_LD && n_iregs < 8) {
      if (ext_insn != NULL) gen_add_insn_before (gen_ctx, call_insn, ext_insn);
      arg_reg_op = _MIR_new_var_op (ctx, R3_HARD_REG + n_iregs);
      if (type != MIR_T_RBLK) {
        gen_mov (gen_ctx, call_insn, MIR_MOV, arg_reg_op, arg_op);
      } else {
        assert (arg_op.mode == MIR_OP_VAR_MEM);
        gen_mov (gen_ctx, call_insn, MIR_MOV, arg_reg_op, _MIR_new_var_op (ctx, arg_op.u.mem.base));
        arg_reg_op = _MIR_new_var_mem_op (ctx, MIR_T_RBLK, arg_op.u.mem.disp, R3_HARD_REG + n_iregs,
                                          MIR_NON_VAR, 1);
      }
      call_insn->ops[i] = arg_reg_op;
    } else { /* put arguments on the stack */
      if (ext_insn != NULL) gen_add_insn_before (gen_ctx, call_insn, ext_insn);
      new_insn_code = (type == MIR_T_F    ? MIR_FMOV
                       : type == MIR_T_D  ? MIR_DMOV
                       : type == MIR_T_LD ? MIR_LDMOV
                                          : MIR_MOV);
      mem_op = _MIR_new_var_mem_op (ctx, mem_type, mem_size + PPC64_STACK_HEADER_SIZE, SP_HARD_REG,
                                    MIR_NON_VAR, 1);
      if (type != MIR_T_RBLK) {
        gen_mov (gen_ctx, call_insn, new_insn_code, mem_op, arg_op);
      } else {
        assert (arg_op.mode == MIR_OP_VAR_MEM);
        gen_mov (gen_ctx, call_insn, new_insn_code, mem_op,
                 _MIR_new_var_op (ctx, arg_op.u.mem.base));
      }
      call_insn->ops[i] = mem_op;
    }
    mem_size += type == MIR_T_LD ? 16 : 8;
    n_iregs += type == MIR_T_LD ? 2 : 1;
  }
  if (vararg_p && mem_size < 64) mem_size = 64; /* to save all arg gprs  */
  if (param_save_area_size < mem_size) param_save_area_size = mem_size;
  n_iregs = n_fregs = 0;
  for (size_t i = 0; i < proto->nres; i++) {
    ret_reg_op = call_insn->ops[i + 2];
    gen_assert (ret_reg_op.mode == MIR_OP_VAR);
    type = proto->res_types[i];
    if (((type == MIR_T_F || type == MIR_T_D) && n_fregs < 4)
        || (type == MIR_T_LD && n_fregs < 3)) {
      new_insn_code = type == MIR_T_F ? MIR_FMOV : type == MIR_T_D ? MIR_DMOV : MIR_LDMOV;
      ret_reg = F1_HARD_REG + n_fregs++;
    } else if (n_iregs < 8) {
      new_insn_code = MIR_MOV;
      ret_reg = R3_HARD_REG + n_iregs++;
    } else {
      (*MIR_get_error_func (ctx)) (MIR_ret_error,
                                   "ppc64 can not handle this combination of return values");
    }
    new_insn = MIR_new_insn (ctx, new_insn_code, ret_reg_op, _MIR_new_var_op (ctx, ret_reg));
    MIR_insert_insn_after (ctx, curr_func_item, call_insn, new_insn);
    call_insn->ops[i + 2] = new_insn->ops[1];
    if ((ext_code = get_ext_code (type)) != MIR_INVALID_INSN) {
      MIR_insert_insn_after (ctx, curr_func_item, new_insn,
                             MIR_new_insn (ctx, ext_code, ret_reg_op, ret_reg_op));
      new_insn = DLIST_NEXT (MIR_insn_t, new_insn);
    }
    create_new_bb_insns (gen_ctx, call_insn, DLIST_NEXT (MIR_insn_t, new_insn), call_insn);
  }
}

static long double mir_i2ld (int64_t i) { return i; }
static const char *I2LD = "mir.i2ld";
static const char *I2LD_P = "mir.i2ld.p";

static long double mir_ui2ld (uint64_t i) { return i; }
static const char *UI2LD = "mir.ui2ld";
static const char *UI2LD_P = "mir.ui2ld.p";

static long double mir_f2ld (float f) { return f; }
static const char *F2LD = "mir.f2ld";
static const char *F2LD_P = "mir.f2ld.p";

static long double mir_d2ld (double d) { return d; }
static const char *D2LD = "mir.d2ld";
static const char *D2LD_P = "mir.d2ld.p";

static int64_t mir_ld2i (long double ld) { return ld; }
static const char *LD2I = "mir.ld2i";
static const char *LD2I_P = "mir.ld2i.p";

static float mir_ld2f (long double ld) { return ld; }
static const char *LD2F = "mir.ld2f";
static const char *LD2F_P = "mir.ld2f.p";

static double mir_ld2d (long double ld) { return ld; }
static const char *LD2D = "mir.ld2d";
static const char *LD2D_P = "mir.ld2d.p";

static long double mir_ldadd (long double d1, long double d2) { return d1 + d2; }
static const char *LDADD = "mir.ldadd";
static const char *LDADD_P = "mir.ldadd.p";

static long double mir_ldsub (long double d1, long double d2) { return d1 - d2; }
static const char *LDSUB = "mir.ldsub";
static const char *LDSUB_P = "mir.ldsub.p";

static long double mir_ldmul (long double d1, long double d2) { return d1 * d2; }
static const char *LDMUL = "mir.ldmul";
static const char *LDMUL_P = "mir.ldmul.p";

static long double mir_lddiv (long double d1, long double d2) { return d1 / d2; }
static const char *LDDIV = "mir.lddiv";
static const char *LDDIV_P = "mir.lddiv.p";

static long double mir_ldneg (long double d) { return -d; }
static const char *LDNEG = "mir.ldneg";
static const char *LDNEG_P = "mir.ldneg.p";

static const char *VA_ARG_P = "mir.va_arg.p";
static const char *VA_ARG = "mir.va_arg";
static const char *VA_BLOCK_ARG_P = "mir.va_block_arg.p";
static const char *VA_BLOCK_ARG = "mir.va_block_arg";

static int64_t mir_ldeq (long double d1, long double d2) { return d1 == d2; }
static const char *LDEQ = "mir.ldeq";
static const char *LDEQ_P = "mir.ldeq.p";

static int64_t mir_ldne (long double d1, long double d2) { return d1 != d2; }
static const char *LDNE = "mir.ldne";
static const char *LDNE_P = "mir.ldne.p";

static int64_t mir_ldlt (long double d1, long double d2) { return d1 < d2; }
static const char *LDLT = "mir.ldlt";
static const char *LDLT_P = "mir.ldlt.p";

static int64_t mir_ldge (long double d1, long double d2) { return d1 >= d2; }
static const char *LDGE = "mir.ldge";
static const char *LDGE_P = "mir.ldge.p";

static int64_t mir_ldgt (long double d1, long double d2) { return d1 > d2; }
static const char *LDGT = "mir.ldgt";
static const char *LDGT_P = "mir.ldgt.p";

static int64_t mir_ldle (long double d1, long double d2) { return d1 <= d2; }
static const char *LDLE = "mir.ldle";
static const char *LDLE_P = "mir.ldle.p";

static int get_builtin (gen_ctx_t gen_ctx, MIR_insn_code_t code, MIR_item_t *proto_item,
                        MIR_item_t *func_import_item) {
  MIR_context_t ctx = gen_ctx->ctx;
  MIR_type_t res_type;

  *func_import_item = *proto_item = NULL; /* to remove uninitialized warning */
  switch (code) {
  case MIR_I2LD:
    res_type = MIR_T_LD;
    *proto_item
      = _MIR_builtin_proto (ctx, curr_func_item->module, I2LD_P, 1, &res_type, 1, MIR_T_I64, "v");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, I2LD, mir_i2ld);
    return 1;
  case MIR_UI2LD:
    res_type = MIR_T_LD;
    *proto_item
      = _MIR_builtin_proto (ctx, curr_func_item->module, UI2LD_P, 1, &res_type, 1, MIR_T_I64, "v");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, UI2LD, mir_ui2ld);
    return 1;
  case MIR_F2LD:
    res_type = MIR_T_LD;
    *proto_item
      = _MIR_builtin_proto (ctx, curr_func_item->module, F2LD_P, 1, &res_type, 1, MIR_T_F, "v");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, F2LD, mir_f2ld);
    return 1;
  case MIR_D2LD:
    res_type = MIR_T_LD;
    *proto_item
      = _MIR_builtin_proto (ctx, curr_func_item->module, D2LD_P, 1, &res_type, 1, MIR_T_D, "v");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, D2LD, mir_d2ld);
    return 1;
  case MIR_LD2I:
    res_type = MIR_T_I64;
    *proto_item
      = _MIR_builtin_proto (ctx, curr_func_item->module, LD2I_P, 1, &res_type, 1, MIR_T_LD, "v");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, LD2I, mir_ld2i);
    return 1;
  case MIR_LD2F:
    res_type = MIR_T_F;
    *proto_item
      = _MIR_builtin_proto (ctx, curr_func_item->module, LD2F_P, 1, &res_type, 1, MIR_T_LD, "v");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, LD2F, mir_ld2f);
    return 1;
  case MIR_LD2D:
    res_type = MIR_T_D;
    *proto_item
      = _MIR_builtin_proto (ctx, curr_func_item->module, LD2D_P, 1, &res_type, 1, MIR_T_LD, "v");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, LD2D, mir_ld2d);
    return 1;
  case MIR_LDADD:
    res_type = MIR_T_LD;
    *proto_item = _MIR_builtin_proto (ctx, curr_func_item->module, LDADD_P, 1, &res_type, 2,
                                      MIR_T_LD, "d1", MIR_T_LD, "d2");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, LDADD, mir_ldadd);
    return 2;
  case MIR_LDSUB:
    res_type = MIR_T_LD;
    *proto_item = _MIR_builtin_proto (ctx, curr_func_item->module, LDSUB_P, 1, &res_type, 2,
                                      MIR_T_LD, "d1", MIR_T_LD, "d2");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, LDSUB, mir_ldsub);
    return 2;
  case MIR_LDMUL:
    res_type = MIR_T_LD;
    *proto_item = _MIR_builtin_proto (ctx, curr_func_item->module, LDMUL_P, 1, &res_type, 2,
                                      MIR_T_LD, "d1", MIR_T_LD, "d2");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, LDMUL, mir_ldmul);
    return 2;
  case MIR_LDDIV:
    res_type = MIR_T_LD;
    *proto_item = _MIR_builtin_proto (ctx, curr_func_item->module, LDDIV_P, 1, &res_type, 2,
                                      MIR_T_LD, "d1", MIR_T_LD, "d2");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, LDDIV, mir_lddiv);
    return 2;
  case MIR_LDNEG:
    res_type = MIR_T_LD;
    *proto_item
      = _MIR_builtin_proto (ctx, curr_func_item->module, LDNEG_P, 1, &res_type, 1, MIR_T_LD, "d");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, LDNEG, mir_ldneg);
    return 1;
  case MIR_LDEQ:
    res_type = MIR_T_I64;
    *proto_item = _MIR_builtin_proto (ctx, curr_func_item->module, LDEQ_P, 1, &res_type, 2,
                                      MIR_T_LD, "d1", MIR_T_LD, "d2");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, LDEQ, mir_ldeq);
    return 2;
  case MIR_LDNE:
    res_type = MIR_T_I64;
    *proto_item = _MIR_builtin_proto (ctx, curr_func_item->module, LDNE_P, 1, &res_type, 2,
                                      MIR_T_LD, "d1", MIR_T_LD, "d2");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, LDNE, mir_ldne);
    return 2;
  case MIR_LDLT:
    res_type = MIR_T_I64;
    *proto_item = _MIR_builtin_proto (ctx, curr_func_item->module, LDLT_P, 1, &res_type, 2,
                                      MIR_T_LD, "d1", MIR_T_LD, "d2");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, LDLT, mir_ldlt);
    return 2;
  case MIR_LDGE:
    res_type = MIR_T_I64;
    *proto_item = _MIR_builtin_proto (ctx, curr_func_item->module, LDGE_P, 1, &res_type, 2,
                                      MIR_T_LD, "d1", MIR_T_LD, "d2");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, LDGE, mir_ldge);
    return 2;
  case MIR_LDGT:
    res_type = MIR_T_I64;
    *proto_item = _MIR_builtin_proto (ctx, curr_func_item->module, LDGT_P, 1, &res_type, 2,
                                      MIR_T_LD, "d1", MIR_T_LD, "d2");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, LDGT, mir_ldgt);
    return 2;
  case MIR_LDLE:
    res_type = MIR_T_I64;
    *proto_item = _MIR_builtin_proto (ctx, curr_func_item->module, LDLE_P, 1, &res_type, 2,
                                      MIR_T_LD, "d1", MIR_T_LD, "d2");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, LDLE, mir_ldle);
    return 2;
  case MIR_VA_ARG:
    res_type = MIR_T_I64;
    *proto_item = _MIR_builtin_proto (ctx, curr_func_item->module, VA_ARG_P, 1, &res_type, 2,
                                      MIR_T_I64, "va", MIR_T_I64, "type");
    *func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, VA_ARG, va_arg_builtin);
    return 2;
  case MIR_VA_BLOCK_ARG:
    *proto_item
      = _MIR_builtin_proto (ctx, curr_func_item->module, VA_BLOCK_ARG_P, 0, NULL, 4, MIR_T_I64,
                            "res", MIR_T_I64, "va", MIR_T_I64, "size", MIR_T_I64, "ncase");
    *func_import_item
      = _MIR_builtin_func (ctx, curr_func_item->module, VA_BLOCK_ARG, va_block_arg_builtin);
    return 4;
  default: return 0;
  }
}

static MIR_disp_t target_get_stack_slot_offset (gen_ctx_t gen_ctx, MIR_type_t type MIR_UNUSED,
                                                MIR_reg_t slot) {
  /* slot is 0, 1, ... */
  return ((MIR_disp_t) slot * 8 + PPC64_STACK_HEADER_SIZE + param_save_area_size);
}

static void set_prev_sp_op (gen_ctx_t gen_ctx, MIR_insn_t anchor, MIR_op_t *prev_sp_op) {
  if (!block_arg_func_p) {
    /* don't use r11 as we can have spilled param<-mem in param set up which needs r11 as a temp */
    block_arg_func_p = TRUE;
    *prev_sp_op = _MIR_new_var_op (gen_ctx->ctx, R12_HARD_REG);
    gen_mov (gen_ctx, anchor, MIR_MOV, *prev_sp_op,
             _MIR_new_var_mem_op (gen_ctx->ctx, MIR_T_I64, 0, SP_HARD_REG, MIR_NON_VAR, 1));
  }
}

static int target_valid_mem_offset_p (gen_ctx_t gen_ctx MIR_UNUSED, MIR_type_t type MIR_UNUSED,
                                      MIR_disp_t offset MIR_UNUSED) {
  return TRUE;
}

static void target_machinize (gen_ctx_t gen_ctx) {
  MIR_context_t ctx = gen_ctx->ctx;
  MIR_func_t func;
  MIR_type_t type, res_type;
  MIR_insn_code_t code, new_insn_code;
  MIR_insn_t insn, next_insn, new_insn, anchor;
  MIR_reg_t ret_reg;
  MIR_op_t ret_reg_op, arg_reg_op, prev_sp_op, temp_op, arg_var_op;
  size_t i, int_arg_num, fp_arg_num, disp, var_args_start, qwords, offset;

  assert (curr_func_item->item_type == MIR_func_item);
  func = curr_func_item->u.func;
  block_arg_func_p = FALSE;
  param_save_area_size = 0;
  anchor = DLIST_HEAD (MIR_insn_t, func->insns);
  if (func->vararg_p)
    set_prev_sp_op (gen_ctx, anchor, &prev_sp_op); /* arg can be taken from memory */
  disp = PPC64_STACK_HEADER_SIZE;                  /* param area start in the caller frame */
  for (i = int_arg_num = fp_arg_num = 0; i < func->nargs; i++) {
    /* Argument extensions is already done in simplify */
    /* Prologue: generate arg_var = hard_reg|stack mem ... */
    type = VARR_GET (MIR_var_t, func->vars, i).type;
    arg_var_op = _MIR_new_var_op (ctx, i + MAX_HARD_REG + 1);
    if ((type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD) && fp_arg_num < 13) {
      if (type == MIR_T_LD && fp_arg_num == 12) { /* dmov f14,disp(r1) -> lfd f14,disp(r1): */
        set_prev_sp_op (gen_ctx, anchor, &prev_sp_op);
        arg_reg_op = _MIR_new_var_op (ctx, F14_HARD_REG);
        gen_mov (gen_ctx, anchor, MIR_DMOV, arg_reg_op,
                 _MIR_new_var_mem_op (ctx, MIR_T_D, disp + 8, R12_HARD_REG, MIR_NON_VAR, 1));
      }
      arg_reg_op = _MIR_new_var_op (ctx, F1_HARD_REG + fp_arg_num);
      gen_mov (gen_ctx, anchor,
               type == MIR_T_F   ? MIR_FMOV
               : type == MIR_T_D ? MIR_DMOV
                                 : MIR_LDMOV,
               arg_var_op, arg_reg_op); /* (f|d|ld|)mov arg, arg_hard_reg */
      fp_arg_num += type == MIR_T_LD ? 2 : 1;
    } else if (type == MIR_T_F || type == MIR_T_D
               || type == MIR_T_LD) { /* (f|d|ld|)mov arg, arg_memory */
      set_prev_sp_op (gen_ctx, anchor, &prev_sp_op);
      gen_mov (gen_ctx, anchor,
               type == MIR_T_F   ? MIR_FMOV
               : type == MIR_T_D ? MIR_DMOV
                                 : MIR_LDMOV,
               arg_var_op, _MIR_new_var_mem_op (ctx, type, disp, R12_HARD_REG, MIR_NON_VAR, 1));
    } else if (MIR_blk_type_p (type)) {
      qwords = (VARR_GET (MIR_var_t, func->vars, i).size + 7) / 8;
      offset = int_arg_num < 8 ? PPC64_STACK_HEADER_SIZE + int_arg_num * 8 : disp;
      set_prev_sp_op (gen_ctx, anchor, &prev_sp_op);
      for (; qwords > 0 && int_arg_num < 8; qwords--, int_arg_num++, disp += 8) {
        if (!func->vararg_p)
          gen_mov (gen_ctx, anchor, MIR_MOV,
                   _MIR_new_var_mem_op (ctx, MIR_T_I64, PPC64_STACK_HEADER_SIZE + int_arg_num * 8,
                                        R12_HARD_REG, MIR_NON_VAR, 1),
                   _MIR_new_var_op (ctx, R3_HARD_REG + int_arg_num));
      }
      gen_add_insn_before (gen_ctx, anchor,
                           MIR_new_insn (ctx, MIR_ADD, arg_var_op,
                                         _MIR_new_var_op (ctx, R12_HARD_REG),
                                         MIR_new_int_op (ctx, offset)));
      disp += qwords * 8;
      int_arg_num += qwords;
      continue;
    } else if (int_arg_num < 8) { /* mov arg, arg_hard_reg */
      arg_reg_op = _MIR_new_var_op (ctx, R3_HARD_REG + int_arg_num);
      gen_mov (gen_ctx, anchor, MIR_MOV, arg_var_op, arg_reg_op);
    } else { /* mov arg, arg_memory */
      set_prev_sp_op (gen_ctx, anchor, &prev_sp_op);
      gen_mov (gen_ctx, anchor, MIR_MOV, arg_var_op,
               _MIR_new_var_mem_op (ctx, MIR_T_I64, disp, R12_HARD_REG, MIR_NON_VAR, 1));
    }
    disp += type == MIR_T_LD ? 16 : 8;
    int_arg_num += type == MIR_T_LD ? 2 : 1;
  }
  var_args_start = disp;
  switch_p = laddr_p = alloca_p = FALSE;
  leaf_p = TRUE;
  for (insn = DLIST_HEAD (MIR_insn_t, func->insns); insn != NULL; insn = next_insn) {
    MIR_item_t proto_item, func_import_item;
    int nargs;

    next_insn = DLIST_NEXT (MIR_insn_t, insn);
    code = insn->code;
    if (code == MIR_LDBEQ || code == MIR_LDBNE || code == MIR_LDBLT || code == MIR_LDBGE
        || code == MIR_LDBGT || code == MIR_LDBLE) { /* split to cmp and branch */
      temp_op = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
      code = (code == MIR_LDBEQ   ? MIR_LDEQ
              : code == MIR_LDBNE ? MIR_LDNE
              : code == MIR_LDBLT ? MIR_LDLT
              : code == MIR_LDBGE ? MIR_LDGE
              : code == MIR_LDBGT ? MIR_LDGT
                                  : MIR_LDLE);
      new_insn = MIR_new_insn (ctx, code, temp_op, insn->ops[1], insn->ops[2]);
      gen_add_insn_before (gen_ctx, insn, new_insn);
      next_insn = MIR_new_insn (ctx, MIR_BT, insn->ops[0], temp_op);
      gen_add_insn_after (gen_ctx, new_insn, next_insn);
      gen_delete_insn (gen_ctx, insn);
      insn = new_insn;
    }
    if ((nargs = get_builtin (gen_ctx, code, &proto_item, &func_import_item)) > 0) {
      if (code == MIR_VA_ARG || code == MIR_VA_BLOCK_ARG) {
        /* Use a builtin func call:
           mov func_reg, func ref; [mov reg3, type;] call proto, func_reg, res_reg, va_reg,
           reg3 */
        MIR_op_t ops[6], func_reg_op, reg_op3;
        MIR_op_t res_reg_op = insn->ops[0], va_reg_op = insn->ops[1], op3 = insn->ops[2];

        assert (res_reg_op.mode == MIR_OP_VAR && va_reg_op.mode == MIR_OP_VAR
                && op3.mode == (code == MIR_VA_ARG ? MIR_OP_VAR_MEM : MIR_OP_VAR));
        func_reg_op = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
        reg_op3 = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
        next_insn = new_insn
          = MIR_new_insn (ctx, MIR_MOV, func_reg_op, MIR_new_ref_op (ctx, func_import_item));
        gen_add_insn_before (gen_ctx, insn, new_insn);
        if (code == MIR_VA_ARG) {
          new_insn
            = MIR_new_insn (ctx, MIR_MOV, reg_op3, MIR_new_int_op (ctx, (int64_t) op3.u.mem.type));
          op3 = reg_op3;
          gen_add_insn_before (gen_ctx, insn, new_insn);
        }
        ops[0] = MIR_new_ref_op (ctx, proto_item);
        ops[1] = func_reg_op;
        ops[2] = res_reg_op;
        ops[3] = va_reg_op;
        ops[4] = op3;
        if (code == MIR_VA_BLOCK_ARG) ops[5] = insn->ops[3];
        new_insn = MIR_new_insn_arr (ctx, MIR_CALL, code == MIR_VA_ARG ? 5 : 6, ops);
        gen_add_insn_before (gen_ctx, insn, new_insn);
        gen_delete_insn (gen_ctx, insn);
      } else { /* Use builtin: mov freg, func ref; call proto, freg, res_reg, op_reg[, op_reg2] */
        MIR_op_t freg_op, res_reg_op = insn->ops[0], op_reg_op = insn->ops[1], ops[5];

        assert (res_reg_op.mode == MIR_OP_VAR && op_reg_op.mode == MIR_OP_VAR);
        freg_op = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
        next_insn = new_insn
          = MIR_new_insn (ctx, MIR_MOV, freg_op, MIR_new_ref_op (ctx, func_import_item));
        gen_add_insn_before (gen_ctx, insn, new_insn);
        ops[0] = MIR_new_ref_op (ctx, proto_item);
        ops[1] = freg_op;
        ops[2] = res_reg_op;
        ops[3] = op_reg_op;
        if (nargs == 2) ops[4] = insn->ops[2];
        new_insn = MIR_new_insn_arr (ctx, MIR_CALL, nargs + 3, ops);
        gen_add_insn_before (gen_ctx, insn, new_insn);
        gen_delete_insn (gen_ctx, insn);
      }
    } else if (code == MIR_VA_START) {
      MIR_op_t treg_op = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
      MIR_op_t treg_op2 = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
      MIR_op_t va_op = insn->ops[0];
      MIR_reg_t va_reg;

      assert (func->vararg_p && va_op.mode == MIR_OP_VAR);
      va_reg = va_op.u.reg;
      /* Insns can be non-simplified as soon as they match a machine insn.  */
      /* treg = mem64[r1]; treg = treg + var_args_start; mem64[va_reg] = treg */
      gen_mov (gen_ctx, insn, MIR_MOV, treg_op,
               _MIR_new_var_mem_op (ctx, MIR_T_I64, 0, R1_HARD_REG, MIR_NON_VAR, 1));
      gen_mov (gen_ctx, insn, MIR_MOV, treg_op2, MIR_new_int_op (ctx, var_args_start));
      /* don't use immediate in ADD as treg_op can become r0: */
      new_insn = MIR_new_insn (ctx, MIR_ADD, treg_op, treg_op, treg_op2);
      gen_add_insn_before (gen_ctx, insn, new_insn);
      gen_mov (gen_ctx, insn, MIR_MOV,
               _MIR_new_var_mem_op (ctx, MIR_T_I64, 0, va_reg, MIR_NON_VAR, 1), treg_op);
      gen_delete_insn (gen_ctx, insn);
    } else if (code == MIR_VA_END) { /* do nothing */
      gen_delete_insn (gen_ctx, insn);
    } else if (MIR_call_code_p (code)) {
      machinize_call (gen_ctx, insn);
      leaf_p = FALSE;
    } else if (code == MIR_ALLOCA) {
      alloca_p = TRUE;
    } else if (code == MIR_SWITCH) {
      switch_p = TRUE;
    } else if (code == MIR_LADDR) {
      laddr_p = TRUE;
    } else if (code == MIR_RET) {
      /* In simplify we already transformed code for one return insn
         and added extension insn (if any).  */
      uint32_t n_gpregs = 0, n_fregs = 0;

      assert (func->nres == MIR_insn_nops (ctx, insn));
      for (i = 0; i < func->nres; i++) {
        assert (insn->ops[i].mode == MIR_OP_VAR);
        res_type = func->res_types[i];
        if (((res_type == MIR_T_F || res_type == MIR_T_D) && n_fregs < 4)
            || (res_type == MIR_T_LD && n_fregs < 3)) {
          new_insn_code = res_type == MIR_T_F   ? MIR_FMOV
                          : res_type == MIR_T_D ? MIR_DMOV
                                                : MIR_LDMOV;
          ret_reg = F1_HARD_REG + n_fregs++;
        } else if (n_gpregs < 8) {
          new_insn_code = MIR_MOV;
          ret_reg = R3_HARD_REG + n_gpregs++;
        } else {
          (*MIR_get_error_func (ctx)) (MIR_ret_error,
                                       "ppc64 can not handle this combination of return values");
        }
        ret_reg_op = _MIR_new_var_op (ctx, ret_reg);
        gen_mov (gen_ctx, insn, new_insn_code, ret_reg_op, insn->ops[i]);
        insn->ops[i] = ret_reg_op;
      }
    }
  }
}

static void isave (gen_ctx_t gen_ctx, MIR_insn_t anchor, int disp, MIR_reg_t hard_reg) {
  gen_mov (gen_ctx, anchor, MIR_MOV,
           _MIR_new_var_mem_op (gen_ctx->ctx, MIR_T_I64, disp, R1_HARD_REG, MIR_NON_VAR, 1),
           _MIR_new_var_op (gen_ctx->ctx, hard_reg));
}

static void fsave (gen_ctx_t gen_ctx, MIR_insn_t anchor, int disp, MIR_reg_t hard_reg) {
  gen_mov (gen_ctx, anchor, MIR_DMOV,
           _MIR_new_var_mem_op (gen_ctx->ctx, MIR_T_D, disp, R1_HARD_REG, MIR_NON_VAR, 1),
           _MIR_new_var_op (gen_ctx->ctx, hard_reg));
}

static void target_make_prolog_epilog (gen_ctx_t gen_ctx, bitmap_t used_hard_regs,
                                       size_t stack_slots_num) {
  MIR_context_t ctx = gen_ctx->ctx;
  MIR_func_t func;
  MIR_insn_t anchor, new_insn;
  MIR_op_t sp_reg_op, fp_reg_op, r0_reg_op, lr_reg_op;
  int64_t start_save_regs_offset;
  size_t i, n, frame_size, saved_iregs_num, saved_fregs_num;

  assert (curr_func_item->item_type == MIR_func_item);
  func = curr_func_item->u.func;
  anchor = DLIST_HEAD (MIR_insn_t, func->insns);
  if (func->vararg_p) {
    for (i = 0; i < 8; i++)
      isave (gen_ctx, anchor, PPC64_STACK_HEADER_SIZE + i * 8, i + R3_HARD_REG);
  }
  for (i = saved_iregs_num = saved_fregs_num = 0; i <= MAX_HARD_REG; i++)
    if (!target_call_used_hard_reg_p (i, MIR_T_UNDEF) && bitmap_bit_p (used_hard_regs, i)) {
      if (i < F0_HARD_REG)
        saved_iregs_num++;
      else
        saved_fregs_num++;
    }
  if (leaf_p && !alloca_p && !switch_p && !laddr_p /* switch and laddr changes LR */
      && saved_iregs_num == 0 && saved_fregs_num == 0 && stack_slots_num == 0)
    return;
  saved_iregs_num++; /* for fp (R31) ??? only alloca_p */
  r0_reg_op = _MIR_new_var_op (ctx, R0_HARD_REG);
  lr_reg_op = _MIR_new_var_op (ctx, LR_HARD_REG);
  sp_reg_op = _MIR_new_var_op (ctx, R1_HARD_REG);
  fp_reg_op = _MIR_new_var_op (ctx, R31_HARD_REG);
  /* Prologue: */
  frame_size = param_save_area_size + PPC64_STACK_HEADER_SIZE + stack_slots_num * 8;
  start_save_regs_offset = frame_size;
  frame_size += (saved_iregs_num + saved_fregs_num) * 8;
  if (frame_size % 16 != 0) frame_size = (frame_size + 15) / 16 * 16;
  if (!func->jret_p) {
    gen_mov (gen_ctx, anchor, MIR_MOV, r0_reg_op, lr_reg_op); /* r0 = lr */
    gen_mov (gen_ctx, anchor, MIR_MOV,
             _MIR_new_var_mem_op (ctx, MIR_T_I64, 16, R1_HARD_REG, MIR_NON_VAR, 1),
             r0_reg_op); /* mem[r1] = r0 */
  }
  gen_mov (gen_ctx, anchor, MIR_MOV, r0_reg_op, sp_reg_op);
  new_insn = MIR_new_insn (ctx, MIR_ADD, sp_reg_op, sp_reg_op, MIR_new_int_op (ctx, -frame_size));
  gen_add_insn_before (gen_ctx, anchor, new_insn); /* r1 -= frame_size */
  gen_mov (gen_ctx, anchor, MIR_MOV,
           _MIR_new_var_mem_op (ctx, MIR_T_I64, 0, R1_HARD_REG, MIR_NON_VAR, 1),
           r0_reg_op); /* mem[r1] = r0 */
  gen_mov (gen_ctx, anchor, MIR_MOV,
           _MIR_new_var_mem_op (ctx, MIR_T_I64, PPC64_TOC_OFFSET, R1_HARD_REG, MIR_NON_VAR, 1),
           _MIR_new_var_op (ctx, R2_HARD_REG)); /* mem[r1+toc_off] = r2 */
  for (n = i = 0; i <= MAX_HARD_REG; i++)
    if (!target_call_used_hard_reg_p (i, MIR_T_UNDEF) && bitmap_bit_p (used_hard_regs, i)) {
      if (i < F0_HARD_REG)
        isave (gen_ctx, anchor, start_save_regs_offset + (n++) * 8, i);
      else
        fsave (gen_ctx, anchor, start_save_regs_offset + (n++) * 8, i);
    }
  isave (gen_ctx, anchor, start_save_regs_offset + n * 8, R31_HARD_REG); /* save R31 */
  gen_mov (gen_ctx, anchor, MIR_MOV, fp_reg_op, sp_reg_op);              /* r31 = r1 */
  /* Epilogue: */
  for (anchor = DLIST_TAIL (MIR_insn_t, func->insns); anchor != NULL;
       anchor = DLIST_PREV (MIR_insn_t, anchor))
    if (anchor->code == MIR_RET || anchor->code == MIR_JRET) break;
  if (anchor == NULL) return;
  /* Restoring hard registers: */
  for (i = n = 0; i <= MAX_HARD_REG; i++)
    if (!target_call_used_hard_reg_p (i, MIR_T_UNDEF) && bitmap_bit_p (used_hard_regs, i)) {
      if (i < F0_HARD_REG) {
        gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, i),
                 _MIR_new_var_mem_op (ctx, MIR_T_I64, start_save_regs_offset + (n++) * 8,
                                      FP_HARD_REG, MIR_NON_VAR, 1));
      } else {
        gen_mov (gen_ctx, anchor, MIR_DMOV, _MIR_new_var_op (ctx, i),
                 _MIR_new_var_mem_op (ctx, MIR_T_D, start_save_regs_offset + (n++) * 8, FP_HARD_REG,
                                      MIR_NON_VAR, 1));
      }
    }
  /* Restore sp, fp, lr */
  new_insn = MIR_new_insn (ctx, MIR_ADD, sp_reg_op, fp_reg_op, MIR_new_int_op (ctx, frame_size));
  gen_add_insn_before (gen_ctx, anchor, new_insn); /* sp = fp + frame_size */
  gen_mov (gen_ctx, anchor, MIR_MOV, fp_reg_op,
           _MIR_new_var_mem_op (ctx, MIR_T_I64, start_save_regs_offset + n * 8, FP_HARD_REG,
                                MIR_NON_VAR, 1)); /* restore fp */
  if (!func->jret_p) {
    gen_mov (gen_ctx, anchor, MIR_MOV, r0_reg_op,
             _MIR_new_var_mem_op (ctx, MIR_T_I64, 16, R1_HARD_REG, MIR_NON_VAR,
                                  1));                        /* r0 = 16(sp) */
    gen_mov (gen_ctx, anchor, MIR_MOV, lr_reg_op, r0_reg_op); /* lr = r0 */
  }
}

struct pattern {
  MIR_insn_code_t code;
  /* Pattern elements:
     blank - ignore
     X - match everything
     $ - finish successfully matching
     r - register but LR
     R - r but R0

     h<one or two decimal digits> - hard register with given number

        memory with signed 16-bit disp and optional non-R0 base:
     m[0-2] - int (signed or unsigned) memory of size 8,16,32,64-bits
     ms[0-2] - signed int type memory of size 8,16,32,64-bits
     mu[0-2] - unsigned int type memory of size 8,16,32,64-bits

       memory with non-R0 base and index:
     M[0-3] - int (signed or unsigned) type memory of size 8,16,32,64-bits
     Ms[0-2] - signed int type memory of size 8,16,32,64-bits
     Mu[0-2] - unsigned int type memory of size 8,16,32,64-bits

     mds - signed 32-bit memory with scaled by 4 signed 16-bit disp and optional non-R0 base:
     Mds - 64-bit memory with scaled by 4 signed 16-bit disp and optional non-R0 base:

     i - 16 bit signed immediate
     I - 16 bit signed immediate shift left by 16
     u - 16 bit unsigned immediate
     U - 16 bit unsigned immediate shift left by 16
     x - 64 bit unsigned immediate whose high 32-bit part is described by pattern 0*1*
     z - 32-bit unsigned immediate
     zs - 32-bit unsigned immediate with zero 0-th bit
     Z - any integer immediate
     Zs - 48-bit unsigned immediate with zero 0-th bit
     Sh - 6-bit unsigned shift
     sh - 5-bit unsigned shift
     ia - roundup (i, 16) as 16 bit signed integer

       memory with signed 16-bit disp and optional non-R0 base:
     mf - memory of float
     md - memory of double
     mld - memory of long double where disp + 8 is also in 16-bit range and non-R0 base reg
     mld0 - as previous bit with R0 base reg
     mds - signed 32-bit memory with scaled by 4 signed 16-bit disp and option non-R0 base:

       memory with non-R0 base and index:
     Mf - memory of float
     Md - memory of double

     L - reference or label as the 1st or 2nd op which can be present by signed 24-bit pc word
     offset

     Remember we have no float or (long) double immediate at this stage.  They were removed during
     simplification.  */

  const char *pattern;
  /* Bit addressing: 0..31
     Replacement elements:
     blank - ignore
     ; - insn separation
     o<number> - opcode [0..5], <number> is decimal
     O<number> - opcode [21..30], <number> is decimal
     P<number> - opcode [26..30], <number> is decimal
     (r|n)t[0-2] - put n-th operand register into rd field [6..10]
     (r|n)s[0-2] - put n-th operand register into rs field [6..10] -- source
     (r|n)a[0-2] - put n-th operand register into rn field [11..15]
     (r|n)b[0-2] - put n-th operand register into rm field [16..20]
     rc[0-2] - put n-th operand register into rc field [21..25]
                   n above means operand reg+1
     Ra[0-2] - put n-th operand register (which is not R0) into rn field [11..15]
     h(t,s,a,b)<dec digits> - hardware register with given number in rt,ra,rb field
     sr<number> - special reg with given number [11..15]
     m = operand is (8-,16-,32-,64-bit) mem with base (0 reg means 0) and signed 16-bit disp
     M = operand is (8-,16-,32-,64-bit) mem with base (0 reg means 0) and index
     mds = 32-bit scaled mem with base (0 reg means 0) and signed 16-bit disp scaled by 4
     Mds = 64-bit scaled mem with base (0 reg means 0) and signed 16-bit disp scaled by 4
     d<number> = field [30..31]
     mt = 8-byte memory -16(r1)
     i - 16 bit signed immediate - field [16..31]
     I - 16 bit signed immediate shift left by 16 - field [16..31]
     u - 16 bit unsigned immediate - field [16..31]
     U - 16 bit unsigned immediate shift left by 16 - field [16..31]
     z[0-3] - n-th 16 bytes of 64-bit immediate
     x - mb for x immediate
     sh<number> - field [16..20]
     Sh<number> - field [16..20,30]
     Mb<number> - field [21..26], and zero bits [27..28]
     Me<number> - field [21..26], and one in bit [27..29]
     Sh - Sh value: field [16..20,30]
     sh - sh value: field [16..20]
     Shr - 64 - Sh value: field [16..20,30]
     shr - 32 - sh value: field [16..20]
     mbSh - mb with value 64-Sh: field [21..26] and zero in bits [27..29]
     mbsh - mb with value 32-sh: field [21..25]
     meSh - mb with value 63-Sh: field [21..26] and 1 in bits [27..29]
     mesh - mb with value 31-sh: field [26..30]
     mb<number> - number in filed [21..25]
     me<number> - number in filed [26..30]
     ia - roundup (i, 16)
     ih - PPC64_STACK_HEADER_SIZE + param_area_size

     bf<number> - field [6..8]
     L - operand-label as 24-bit word pc offset scaled by 4: field [6..29]
     l - operand-label as 14-bit word pc offset scaled by 4: field [16..29]
     l<number> - operand-label as 14-bit word pc offset scaled by 4: field [16..29]
     L<number> - long data bitfield [10..10]
     BO<number> - field [6..10] for bcond
     BI<number> - field [11..15] for bcond
     LK<number> - field [31..31]

     W - LADDR label which is a 32-bit signed offset from previous insn
     at - address disp PPC64_TOC_OFFSET
     T - switch table displacement
  */
  const char *replacement;
};

static const struct pattern patterns[] = {
  {MIR_MOV, "r r", "o31 O444 ra0 rs1 rb1"}, /* or ra,rs,rs */
  {MIR_MOV, "r h64", "o31 O339 rt0 sr8"},   /* mflr rt */
  {MIR_MOV, "h64 r", "o31 O467 rs1 sr8"},   /* mtlr rs */

  // ??? loads/stores with update
  {MIR_MOV, "r Mds", "o58 rt0 Mds"},   /* ld rt,ds-mem */
  {MIR_MOV, "Mds r", "o62 rs1 Mds"},   /* std rt,ds-mem */
  {MIR_MOV, "r M3", "o31 O21 rt0 M"},  /* ldx rt,index-mem */
  {MIR_MOV, "M3 r", "o31 O149 rs1 M"}, /* stdx rs,index-mem */

  {MIR_MOV, "r mu2", "o32 rt0 m"},     /* lwz rt,disp-mem */
  {MIR_MOV, "m2 r", "o36 rs1 m"},      /* stw rs,disp-mem */
  {MIR_MOV, "r Mu2", "o31 O23 rt0 M"}, /* lwzx rt,index-mem */
  {MIR_MOV, "M2 r", "o31 O151 rs1 M"}, /* stwx rs,index-mem */

  {MIR_MOV, "r mds", "o58 rt0 mds d2"}, /* lwa rt,ds-mem */
  {MIR_MOV, "r Ms2", "o31 O341 rt0 M"}, /* lwax rt,index-mem */

  {MIR_MOV, "r mu1", "o40 rt0 m"},      /* lhz rt,disp-mem */
  {MIR_MOV, "m1 r", "o44 rs1 m"},       /* sth rs,disp-mem */
  {MIR_MOV, "r Mu1", "o31 O279 rt0 M"}, /* lhzx rt,index-mem */
  {MIR_MOV, "M1 r", "o31 O407 rs1 M"},  /* sthx rs,index-mem */

  {MIR_MOV, "r ms1", "o42 rt0 m"},      /* lha rt,disp-mem */
  {MIR_MOV, "r Ms1", "o31 O343 rt0 M"}, /* lhax rt,index-mem */

  {MIR_MOV, "r mu0", "o34 rt0 m"},     /* lbz rt,disp-mem */
  {MIR_MOV, "m0 r", "o38 rs1 m"},      /* stb rs,disp-mem */
  {MIR_MOV, "r Mu0", "o31 O87 rt0 M"}, /* lbzx rt,index-mem */
  {MIR_MOV, "M0 r", "o31 O215 rs1 M"}, /* stbx rs,index-mem */

  {MIR_MOV, "r ms0", "o34 rt0 m; o31 O954 rs0 ra0"},     /* lbz rt,disp-mem; extsb rt,rt */
  {MIR_MOV, "r Ms0", "o31 O87 rt0 M; o31 O954 rs0 ra0"}, /* lbzx rt,index-mem; extsb rt,rt */

  {MIR_MOV, "r i", "o14 rt0 ha0 i"},                   /* li rt,i == addi rt,0,i */
  {MIR_MOV, "r I", "o15 rt0 ha0 I"},                   /* lis rt,i == addis rt,0,i */
  {MIR_MOV, "r zs", "o15 rt0 ha0 z2; o24 rt0 ra0 z3"}, /* lis rt,z2; ori rt,rt,z3 */
  /* lis rt,rt,z2; ori rt,rt,z3; clrdi rt,rt,X */
  {MIR_MOV, "r x", "o15 rt0 ha0 z2; o24 ra0 rs0 z3; o30 ra0 rs0 Sh0 x"},
  /* xor rt,rt,rt; oris rt,rt,z2; ori rt,rt,z3: */
  {MIR_MOV, "r z", "o31 O316 rs0 ra0 rb0; o25 ra0 rs0 z2; o24 ra0 rs0 z3"},
  /* li rt,r0,z1; rldicr rt,rt,32,31; oris rt,rt,z2; ori rt,rt,z3 */
  {MIR_MOV, "r Zs", "o14 rt0 ha0 z1; o30 rt0 ra0 Sh32 Me31; o25 ra0 rs0 z2; o24 ra0 rs0 z3"},
  /* lis rt,r0,z0; ori rt,rt,z1; rldicr rt,rt,32,31; oris rt,rt,z2; ori rt,rt,z3: */
  {MIR_MOV, "r Z",
   "o15 rt0 ha0 z0; o24 ra0 rs0 z1; o30 rt0 ra0 Sh32 Me31; o25 ra0 rs0 z2; o24 ra0 rs0 z3"},

  {MIR_FMOV, "r r", "o63 O72 rt0 rb1"}, /*  fmr rt,rb */
  {MIR_FMOV, "r mf", "o48 rt0 m"},      /* lfs rt, disp-mem */
  {MIR_FMOV, "r Mf", "o31 O535 rt0 M"}, /* lfsx rt, index-mem */
  {MIR_FMOV, "mf r", "o52 rt1 m"},      /* stfs rt, disp-mem */
  {MIR_FMOV, "Mf r", "o31 O663 rt1 M"}, /* stfsx rt, index-mem */

  {MIR_DMOV, "r r", "o63 O72 rt0 rb1"}, /*  fmr rt,rb */
  {MIR_DMOV, "r md", "o50 rt0 m"},      /* lds rt, disp-mem */
  {MIR_DMOV, "r Md", "o31 O599 rt0 M"}, /* lfdx rt, index-mem */
  {MIR_DMOV, "md r", "o54 rt1 m"},      /* stfd rt, disp-mem */
  {MIR_DMOV, "Md r", "o31 O727 rt1 M"}, /* stfdx rt, index-mem */

  {MIR_LDMOV, "r r", "o63 O72 rt0 rb1;o63 O72 nt0 nb1"}, /* fmr rt,rb; fmr rt+1,rb+1 */
  {MIR_LDMOV, "r mld", "o50 rt0 m; o50 nt0 mn"},         /* lfd rt,disp-mem; lfd rt+1,disp+8-mem */
  {MIR_LDMOV, "mld r", "o54 rt1 m; o54 nt1 mn"}, /* stfd rt,disp-mem; stfdx rt+1,disp+8-mem */
  {MIR_LDMOV, "r mld0",
   "o31 O444 ha11 hs0 hb0; o50 rt0 ha11; o50 nt0 ha11 i8"}, /* mr r11,r0; lfd rt,(r11); lfd
                                                               rt+1,8(r11) */
  {MIR_LDMOV, "mld0 r",
   "o31 O444 ha11 hs0 hb0; o54 rt1 ha11; o54 nt1 ha11 i8"}, /* mr r11,r0; stfd rt,(r11); stfdx
                                                               rt+1,8(r11) */

  {MIR_EXT8, "r r", "o31 O954 ra0 rs1"},  /* extsb ra,rs */
  {MIR_EXT16, "r r", "o31 O922 ra0 rs1"}, /* extsh ra,rs */
  {MIR_EXT32, "r r", "o31 O986 ra0 rs1"}, /* extsw ra,rs */

  {MIR_UEXT8, "r r", "o30 ra0 rs1 Sh0 Mb56"},  /* rldicl ra,rs,0,56 */
  {MIR_UEXT16, "r r", "o30 ra0 rs1 Sh0 Mb48"}, /* rldicl ra,rs,0,48 */
  {MIR_UEXT32, "r r", "o30 ra0 rs1 Sh0 Mb32"}, /* rldicl ra,rs,0,32 */

  {MIR_ADD, "r r r", "o31 O266 rt0 ra1 rb2"},  /* add rt,ra,rb */
  {MIR_ADD, "r R i", "o14 rt0 ra1 i"},         /* addi rt,ra,i */
  {MIR_ADD, "r R I", "o15 rt0 ra1 I"},         /* addis rt,ra,I */
  {MIR_ADDS, "r r r", "o31 O266 rt0 ra1 rb2"}, /* add rt,ra,rb */
  {MIR_ADDS, "r R i", "o14 rt0 ra1 i"},        /* addi rt,ra,i */
  {MIR_ADDS, "r R I", "o15 rt0 ra1 I"},        /* addis rt,ra,I */
  {MIR_FADD, "r r r", "o59 O21 rt0 ra1 rb2"},  /* fadds rt,ra,rb*/
  {MIR_DADD, "r r r", "o63 O21 rt0 ra1 rb2"},  /* fadd rt,ra,rb*/
  // ldadd is implemented through builtin

  // ??? transform sub immediate to add immediate
  {MIR_SUB, "r r r", "o31 O40 rt0 rb1 ra2"},  /* subf rt,ra,rb */
  {MIR_SUBS, "r r r", "o31 O40 rt0 rb1 ra2"}, /* subf rt,ra,rb */
  {MIR_FSUB, "r r r", "o59 O20 rt0 ra1 rb2"}, /* fsubs rt,ra,rb*/
  {MIR_DSUB, "r r r", "o63 O20 rt0 ra1 rb2"}, /* fsub rt,ra,rb*/
  // ldsub is implemented through builtin

  {MIR_MUL, "r r r", "o31 O233 rt0 ra1 rb2"},  /* mulld rt,ra,rb*/
  {MIR_MUL, "r r i", "o7 rt0 ra1 i"},          /* mulli rt,ra,i*/
  {MIR_MULS, "r r r", "o31 O235 rt0 ra1 rb2"}, /* mullw rt,ra,rb*/
  {MIR_FMUL, "r r r", "o59 P25 rt0 ra1 rc2"},  /* fmuls rt,ra,rc*/
  {MIR_DMUL, "r r r", "o63 P25 rt0 ra1 rc2"},  /* fmul rt,ra,rc*/
  // ldmul is implemented through builtin

  {MIR_DIV, "r r r", "o31 O489 rt0 ra1 rb2"},   /* divd rt,ra,rb*/
  {MIR_DIVS, "r r r", "o31 O491 rt0 ra1 rb2"},  /* divw rt,ra,rb*/
  {MIR_UDIV, "r r r", "o31 O457 rt0 ra1 rb2"},  /* divdu rt,ra,rb*/
  {MIR_UDIVS, "r r r", "o31 O459 rt0 ra1 rb2"}, /* divwu rt,ra,rb*/
  {MIR_FDIV, "r r r", "o59 O18 rt0 ra1 rb2"},   /* fdivs rt,ra,rb*/
  {MIR_DDIV, "r r r", "o63 O18 rt0 ra1 rb2"},   /* fdiv rt,ra,rb*/
  // lddiv is implemented through builtin

  /* divd r10,ra,rb;muld r10,r10,rb;subf r,r10,ra: */
  {MIR_MOD, "r r r", "o31 O489 ht10 ra1 rb2; o31 O233 ht10 ha10 rb2; o31 O40 rt0 ha10 rb1"},
  /* divw r10,ra,rb;mulw r10,r10,rb;subf r,r10,ra: */
  {MIR_MODS, "r r r", "o31 O491 ht10 ra1 rb2; o31 O235 ht10 ha10 rb2; o31 O40 rt0 ha10 rb1"},
  /* divdu r10,ra,rb;muld r10,r10,rb;subf r,r10,ra: */
  {MIR_UMOD, "r r r", "o31 O457 ht10 ra1 rb2; o31 O233 ht10 ha10 rb2; o31 O40 rt0 ha10 rb1"},
  /* divwu r10,ra,rb;mulw r10,r10,rb;subf r,r10,ra: */
  {MIR_UMODS, "r r r", "o31 O459 ht10 ra1 rb2; o31 O235 ht10 ha10 rb2; o31 O40 rt0 ha10 rb1"},

#define MFCR "o31 O19 rt0"
#define EQEND MFCR "; o21 rs0 ra0 sh31 mb31 me31"
#define CMPD "o31 O0 bf7 L1 ra1 rb2"
#define CMPDI(i) "o11 bf7 L1 ra1 " #i
#define CMPW "o31 O0 bf7 L0 ra1 rb2"
#define CMPWI(i) "o11 bf7 L0 ra1 " #i
#define FCMPU "o63 O0 bf7 ra1 rb2"
#define CRNOT(s, f) "o19 O33 ht" #s " ha" #f " hb" #f ";"
#define CROR(t, a, b) "o19 O449 ht" #t " ha" #a " hb" #b ";"
#define CRORC(t, a, b) "o19 O417 ht" #t " ha" #a " hb" #b ";"
#define CRNOR(t, a, b) "o19 O33 ht" #t " ha" #a " hb" #b ";"
#define CRANDC(t, a, b) "o19 O129 ht" #t " ha" #a " hb" #b ";"
  // all ld insn are changed to builtins
  /* cmpd 7,ra,rb; mfcr rt; rlwinm rt,rt,31,31,31;*/
  {MIR_EQ, "r r r", CMPD "; " EQEND},
  /* cmpdi 7,ra,i; mfcr rt; rlwinm rt,rt,31,31,31*/
  {MIR_EQ, "r r i", CMPDI (i) "; " EQEND},
  /* cmpw 7,ra,rb; mfcr rt; rlwinm rt,rt,31,31,31;*/
  {MIR_EQS, "r r r", CMPW "; " EQEND},
  /* cmpwi 7,ra,i; mfcr rt; rlwinm rt,rt,31,31,31*/
  {MIR_EQS, "r r i", CMPWI (i) "; " EQEND},
  /* fcmpu 7,ra,rb; mfcr rt; crandc 30,30,31; rlwinm rt,rt,31,31,31;*/
  {MIR_FEQ, "r r r", FCMPU ";" CRANDC (30, 30, 31) EQEND},
  /* fcmpu 7,ra,rb; mfcr rt; crandc 30,30,31; rlwinm rt,rt,31,31,31;*/
  {MIR_DEQ, "r r r", FCMPU ";" CRANDC (30, 30, 31) EQEND},

#define NEEND EQEND "; o26 rs0 ra0 i1"
  /* cmpd 7,ra,rb; mfcr rt; rlwinm rt,rt,31,31,31; xori rt,rt,1*/
  {MIR_NE, "r r r", CMPD "; " NEEND},
  /* cmpdi 7,ra,i; mfcr rt; rlwinm rt,rt,31,31,31; xori rt,rt,1*/
  {MIR_NE, "r r i", CMPDI (i) "; " NEEND},
  /* cmpw 7,ra,rb; mfcr rt; rlwinm rt,rt,31,31,31; xori rt,rt,1*/
  {MIR_NES, "r r r", CMPW "; " NEEND},
  /* cmpwi 7,ra,i; mfcr rt; rlwinm rt,rt,31,31,31; xori rt,rt,1*/
  {MIR_NES, "r r i", CMPWI (i) "; " NEEND},
  /* fcmpu 7,ra,rb; crorc 30,31,30; mfcr rt; rlwinm rt,rt,31,31,31;*/
  {MIR_FNE, "r r r", FCMPU "; " CRORC (30, 31, 30) EQEND},
  /* fcmpu 7,ra,rb; crorc 30,31,30; mfcr rt; rlwinm rt,rt,31,31,31;*/
  {MIR_DNE, "r r r", FCMPU "; " CRORC (30, 31, 30) EQEND},

#define RLWINM(n) "o21 rs0 ra0 sh" #n " mb31 me31"
  /* cmpd 7,ra,rb; mfcr rt; rlwinm rt,rt,29,31,31;*/
  {MIR_LT, "r r r", CMPD "; " MFCR ";  " RLWINM (29)},
  /* cmpdi 7,ra,i; mfcr rt; rlwinm rt,rt,29,31,31*/
  {MIR_LT, "r r i", CMPDI (i) "; " MFCR ";  " RLWINM (29)},
  /* cmpw 7,ra,rb; mfcr rt; rlwinm rt,rt,29,31,31;*/
  {MIR_LTS, "r r r", CMPW "; " MFCR ";  " RLWINM (29)},
  /* cmpwi 7,ra,i; mfcr rt; rlwinm rt,rt,29,31,31*/
  {MIR_LTS, "r r i", CMPWI (i) "; " MFCR ";  " RLWINM (29)},
  /* fcmpu 7,ra,rb; crandc 28,28,31; mfcr rt; rlwinm rt,rt,29,31,31;*/
  {MIR_FLT, "r r r", FCMPU "; " CRANDC (28, 28, 31) MFCR "; " RLWINM (29)},
  /* fcmpu 7,ra,rb; crandc 28,28,31; mfcr rt; rlwinm rt,rt,29,31,31;*/
  {MIR_DLT, "r r r", FCMPU "; " CRANDC (28, 28, 31) MFCR "; " RLWINM (29)},

#define CMPLD "o31 O32 bf7 L1 ra1 rb2"
#define CMPLDI "o10 bf7 L1 ra1 u"
#define CMPLW "o31 O32 bf7 L0 ra1 rb2"
#define CMPLWI "o10 bf7 L0 ra1 u"
  /* cmpld 7,ra,rb; mfcr rt; rlwinm rt,rt,29,31,31;*/
  {MIR_ULT, "r r r", CMPLD "; " MFCR ";  " RLWINM (29)},
  /* cmpldi 7,ra,u; mfcr rt; rlwinm rt,rt,29,31,31;*/
  {MIR_ULT, "r r u", CMPLDI "; " MFCR ";  " RLWINM (29)},
  /* cmplw 7,ra,rb; mfcr rt; rlwinm rt,rt,29,31,31;*/
  {MIR_ULTS, "r r r", CMPLW "; " MFCR ";  " RLWINM (29)},
  /* cmplwi 7,ra,u; mfcr rt; rlwinm rt,rt,29,31,31;*/
  {MIR_ULTS, "r r u", CMPLWI "; " MFCR ";  " RLWINM (29)},

  /* cmpd 7,ra,rb; crnot 28,28; mfcr rt; rlwinm rt,rt,29,31,31;*/
  {MIR_GE, "r r r", CMPD "; " CRNOT (28, 28) MFCR ";  " RLWINM (29)},
  /* cmpdi 7,ra,i; crnot 28,28; mfcr rt; rlwinm rt,rt,29,31,31*/
  {MIR_GE, "r r i", CMPDI (i) "; " CRNOT (28, 28) MFCR ";  " RLWINM (29)},
  /* cmpw 7,ra,rb; crnot 28,28; mfcr rt; rlwinm rt,rt,29,31,31;*/
  {MIR_GES, "r r r", CMPW "; " CRNOT (28, 28) MFCR ";  " RLWINM (29)},
  /* cmpwi 7,ra,i; crnot 28,28; mfcr rt; rlwinm rt,rt,29,31,31*/
  {MIR_GES, "r r i", CMPWI (i) "; " CRNOT (28, 28) MFCR ";  " RLWINM (29)},
  /* fcmpu 7,ra,rb; crnot 28,28,31; mfcr rt; rlwinm rt,rt,29,31,31;*/
  {MIR_FGE, "r r r", FCMPU "; " CRNOR (28, 28, 31) MFCR ";  " RLWINM (29)},
  /* fcmpu 7,ra,rb; crnor 28,28,31; mfcr rt; rlwinm rt,rt,29,31,31;*/
  {MIR_DGE, "r r r", FCMPU "; " CRNOR (28, 28, 31) MFCR ";  " RLWINM (29)},
  /* cmpld 7,ra,rb; crnot 28,28; mfcr rt; rlwinm rt,rt,29,31,31;*/
  {MIR_UGE, "r r r", CMPLD "; " CRNOT (28, 28) MFCR ";  " RLWINM (29)},
  /* cmpldi 7,ra,u; crnot 28,28; mfcr rt; rlwinm rt,rt,29,31,31;*/
  {MIR_UGE, "r r u", CMPLDI "; " CRNOT (28, 28) MFCR ";  " RLWINM (29)},
  /* cmplw 7,ra,rb; crnot 28,28; mfcr rt; rlwinm rt,rt,29,31,31;*/
  {MIR_UGES, "r r r", CMPLW "; " CRNOT (28, 28) MFCR ";  " RLWINM (29)},
  /* cmplwi 7,ra,u; crnot 28,28; mfcr rt; rlwinm rt,rt,29,31,31;*/
  {MIR_UGES, "r r u", CMPLWI "; " CRNOT (28, 28) MFCR ";  " RLWINM (29)},

  /* cmpd 7,ra,rb; mfcr rt; rlwinm rt,rt,30,31,31;*/
  {MIR_GT, "r r r", CMPD "; " MFCR ";  " RLWINM (30)},
  /* cmpdi 7,ra,i; mfcr rt; rlwinm rt,rt,30,31,31*/
  {MIR_GT, "r r i", CMPDI (i) "; " MFCR ";  " RLWINM (30)},
  /* cmpw 7,ra,rb; mfcr rt; rlwinm rt,rt,30,31,31;*/
  {MIR_GTS, "r r r", CMPW "; " MFCR ";  " RLWINM (30)},
  /* cmpwi 7,ra,i; mfcr rt; rlwinm rt,rt,30,31,31*/
  {MIR_GTS, "r r i", CMPWI (i) "; " MFCR ";  " RLWINM (30)},
  /* fcmpu 7,ra,rb; crandc 29,29,30; mfcr rt; rlwinm rt,rt,30,31,31;*/
  {MIR_FGT, "r r r", FCMPU "; " CRANDC (29, 29, 31) MFCR "; " RLWINM (30)},
  /* fcmpu 7,ra,rb; crandc 29,29,30; mfcr rt; rlwinm rt,rt,30,31,31;*/
  {MIR_DGT, "r r r", FCMPU "; " CRANDC (29, 29, 31) MFCR ";  " RLWINM (30)},
  /* cmpld 7,ra,rb; mfcr rt; rlwinm rt,rt,30,31,31;*/
  {MIR_UGT, "r r r", CMPLD "; " MFCR ";  " RLWINM (30)},
  /* cmpldi 7,ra,u; mfcr rt; rlwinm rt,rt,30,31,31;*/
  {MIR_UGT, "r r u", CMPLDI "; " MFCR ";  " RLWINM (30)},
  /* cmplw 7,ra,rb; mfcr rt; rlwinm rt,rt,30,31,31;*/
  {MIR_UGTS, "r r r", CMPLW "; " MFCR ";  " RLWINM (30)},
  /* cmplwi 7,ra,u; mfcr rt; rlwinm rt,rt,30,31,31;*/
  {MIR_UGTS, "r r u", CMPLWI "; " MFCR ";  " RLWINM (30)},

  /* cmpd 7,ra,rb; crnot 29,29; mfcr rt; rlwinm rt,rt,30,31,31;*/
  {MIR_LE, "r r r", CMPD "; " CRNOT (29, 29) MFCR ";  " RLWINM (30)},
  /* cmpdi 7,ra,i; crnot 29,29; mfcr rt; rlwinm rt,rt,30,31,31*/
  {MIR_LE, "r r i", CMPDI (i) "; " CRNOT (29, 29) MFCR ";  " RLWINM (30)},
  /* cmpw 7,ra,rb; crnot 29,29; mfcr rt; rlwinm rt,rt,30,31,31;*/
  {MIR_LES, "r r r", CMPW "; " CRNOT (29, 29) MFCR ";  " RLWINM (30)},
  /* cmpwi 7,ra,i; crnot 29,29; mfcr rt; rlwinm rt,rt,30,31,31*/
  {MIR_LES, "r r i", CMPWI (i) "; " CRNOT (29, 29) MFCR ";  " RLWINM (30)},
  /* fcmpu 7,ra,rb; crnor 29,29,31; mfcr rt; rlwinm rt,rt,30,31,31;*/
  {MIR_FLE, "r r r", FCMPU "; " CRNOR (29, 29, 31) MFCR ";  " RLWINM (30)},
  /* fcmpu 7,ra,rb; crnor 29,29,31; mfcr rt; rlwinm rt,rt,30,31,31;*/
  {MIR_DLE, "r r r", FCMPU "; " CRNOR (29, 29, 31) MFCR ";  " RLWINM (30)},
  /* cmpld 7,ra,rb; crnot 29,29; mfcr rt; rlwinm rt,rt,30,31,31;*/
  {MIR_ULE, "r r r", CMPLD "; " CRNOT (29, 29) MFCR ";  " RLWINM (30)},
  /* cmpldi 7,ra,u; crnot 29,29; mfcr rt; rlwinm rt,rt,30,31,31;*/
  {MIR_ULE, "r r u", CMPLDI "; " CRNOT (29, 29) MFCR ";  " RLWINM (30)},
  /* cmplw 7,ra,rb; crnot 29,29; mfcr rt; rlwinm rt,rt,30,31,31;*/
  {MIR_ULES, "r r r", CMPLW "; " CRNOT (29, 29) MFCR ";  " RLWINM (30)},
  /* cmplwi 7,ra,u; crnot 29,29; mfcr rt; rlwinm rt,rt,30,31,31;*/
  {MIR_ULES, "r r u", CMPLWI "; " CRNOT (29, 29) MFCR ";  " RLWINM (30)},

  {MIR_JMP, "L", "o18 L"}, /* 24-bit offset word jmp */

  /* bl l4; mflr r0; addis r0,r0,I; addi r0,r0,i: */
  {MIR_LADDR, "r W", "o18 l4 LK1; o31 O339 rt0 sr8; o15 rt0 ra0 W; o14 rt0 ra0"},
  {MIR_JMPI, "r", "o31 O467 rs0 sr9; o19 O528 BO20 BI0"}, /* mtctr r; bcctr */

#define BRC(o, i) "o16 BO" #o " BI" #i " l"
#define BRCL(o, i) "o16 BO" #o " BI" #i " l8; o18 L"
#define BRLOG(CODE, CMP, BI, COND, NEG_COND)                                \
  {CODE, "l r", CMP (i0) "; " BRC (COND, BI)}, /* cmp 7,ra1,0;bcond 7,l; */ \
  {                                                                         \
    CODE, "L r", CMP (i0) "; " BRCL (NEG_COND, BI)                          \
  } /* cmp 7,ra1,0;bneg_cond 7,o8;b L;l8:*/

  BRLOG (MIR_BT, CMPDI, 30, 4, 12),
  BRLOG (MIR_BTS, CMPWI, 30, 4, 12),
  BRLOG (MIR_BF, CMPDI, 30, 12, 4),
  BRLOG (MIR_BFS, CMPWI, 30, 12, 4),

#define BRCMP(CODE, CMP, CMPI, BI, COND, NEG_COND)                                           \
  {CODE, "l r r", CMP "; " BRC (COND, BI)},        /* cmp 7,ra1,rb2;bcond 7,l; */            \
    {CODE, "l r i", CMPI "; " BRC (COND, BI)},     /* cmpi 7,ra1,i;bcond 7,l;:*/             \
    {CODE, "L r r", CMP "; " BRCL (NEG_COND, BI)}, /* cmp 7,ra1,rb2;bneg_cond 7,o8;b L;l8:*/ \
  {                                                                                          \
    CODE, "L r i", CMPI "; " BRCL (NEG_COND, BI)                                             \
  } /* cmp 7,ra1,i;bneg_cond 7,o8;b L;l8:*/

#define BRFCMP(CODE, BI, COND, COND_NAND, NEG_COND, NEG_COND_NAND)                     \
  {CODE, "l r r", FCMPU "; " COND_NAND BRC (COND, BI)}, /* cmp 7,ra1,rb2;bcond 7,l; */ \
  {                                                                                    \
    CODE, "L r r", FCMPU "; " NEG_COND_NAND BRCL (NEG_COND, BI)                        \
  } /* cmp 7,ra1,rb2;bneg_cond 7,o8;b L;l8:*/

#define LT_OR CROR (28, 28, 31)
#define GT_OR CROR (29, 29, 31)
#define EQ_OR CROR (30, 30, 31)

#define LT_ANDC CRANDC (28, 28, 31)
#define GT_ANDC CRANDC (29, 29, 31)
#define EQ_ANDC CRANDC (30, 30, 31)

  // all ld insn are changed to builtins and bt/bts
  BRCMP (MIR_BEQ, CMPD, CMPDI (i), 30, 12, 4),
  BRCMP (MIR_BEQS, CMPW, CMPWI (i), 30, 12, 4),
  BRFCMP (MIR_FBEQ, 30, 12, EQ_ANDC, 4, EQ_OR),
  BRFCMP (MIR_DBEQ, 30, 12, EQ_ANDC, 4, EQ_OR),

  BRCMP (MIR_BNE, CMPD, CMPDI (i), 30, 4, 12),
  BRCMP (MIR_BNES, CMPW, CMPWI (i), 30, 4, 12),
  BRFCMP (MIR_FBNE, 30, 4, EQ_ANDC, 12, EQ_ANDC),
  BRFCMP (MIR_DBNE, 30, 4, EQ_ANDC, 12, EQ_ANDC),

#define BRUCMP(CODE, CMP, CMPI, BI, COND, NEG_COND)                                          \
  {CODE, "l r r", CMP "; " BRC (COND, BI)},        /* cmp 7,ra1,rb2;bcond 7,l; */            \
    {CODE, "l r u", CMPI "; " BRC (COND, BI)},     /* cmpi 7,ra1,u;bcond 7,l;:*/             \
    {CODE, "L r r", CMP "; " BRCL (NEG_COND, BI)}, /* cmp 7,ra1,rb2;bneg_cond 7,o8;b L;l8:*/ \
  {                                                                                          \
    CODE, "L r u", CMPI "; " BRCL (NEG_COND, BI)                                             \
  } /* cmp 7,ra1,u;bneg_cond 7,o8;b L;l8:*/

  /* LT: */
  BRCMP (MIR_BLT, CMPD, CMPDI (i), 28, 12, 4),
  BRCMP (MIR_BLTS, CMPW, CMPWI (i), 28, 12, 4),
  BRFCMP (MIR_FBLT, 28, 12, LT_ANDC, 4, LT_OR),
  BRFCMP (MIR_DBLT, 28, 12, LT_ANDC, 4, LT_OR),
  BRUCMP (MIR_UBLT, CMPLD, CMPLDI, 28, 12, 4),
  BRUCMP (MIR_UBLTS, CMPLW, CMPLWI, 28, 12, 4),

  /* GE: */
  BRCMP (MIR_BGE, CMPD, CMPDI (i), 28, 4, 12),
  BRCMP (MIR_BGES, CMPW, CMPWI (i), 28, 4, 12),
  BRFCMP (MIR_FBGE, 28, 4, LT_OR, 12, LT_ANDC),
  BRFCMP (MIR_DBGE, 28, 4, LT_OR, 12, LT_ANDC),
  BRUCMP (MIR_UBGE, CMPLD, CMPLDI, 28, 4, 12),
  BRUCMP (MIR_UBGES, CMPLW, CMPLWI, 28, 4, 12),

  /* GT: */
  BRCMP (MIR_BGT, CMPD, CMPDI (i), 29, 12, 4),
  BRCMP (MIR_BGTS, CMPW, CMPWI (i), 29, 12, 4),
  BRFCMP (MIR_FBGT, 29, 12, GT_ANDC, 4, GT_OR),
  BRFCMP (MIR_DBGT, 29, 12, GT_ANDC, 4, GT_OR),
  BRUCMP (MIR_UBGT, CMPLD, CMPLDI, 29, 12, 4),
  BRUCMP (MIR_UBGTS, CMPLW, CMPLWI, 29, 12, 4),

  /* LE: */
  BRCMP (MIR_BLE, CMPD, CMPDI (i), 29, 4, 12),
  BRCMP (MIR_BLES, CMPW, CMPWI (i), 29, 4, 12),
  BRFCMP (MIR_FBLE, 29, 4, GT_OR, 12, GT_ANDC),
  BRFCMP (MIR_DBLE, 29, 4, GT_OR, 12, GT_ANDC),
  BRUCMP (MIR_UBLE, CMPLD, CMPLDI, 29, 4, 12),
  BRUCMP (MIR_UBLES, CMPLW, CMPLWI, 29, 4, 12),

#define NEG "o31 O104 rt0 ra1"
#define FNEG "o63 O40 rt0 rb1"

  {MIR_NEG, "r r", NEG},   /* neg Rt,Ra */
  {MIR_NEGS, "r r", NEG},  /* neg Rt,Ra */
  {MIR_FNEG, "r r", FNEG}, /* fneg rt,rb */
  {MIR_DNEG, "r r", FNEG}, /* fneg rt,rb */
// ldneg is a builtin

#define SHR(s) "o31 O" #s " ra0 rs1 rb2"
  {MIR_LSH, "r r r", SHR (27)},                    /* sld ra,rs,rb */
  {MIR_LSHS, "r r r", SHR (24)},                   /* slw ra,rs,rb */
  {MIR_LSH, "r r Sh", "o30 ra0 rs1 Sh meSh"},      /* rldicr ra,rs,sh,63-sh */
  {MIR_LSHS, "r r sh", "o21 ra0 rs1 sh mb0 mesh"}, /* rlwinm ra,rs,sh,0,31-sh */

  {MIR_RSH, "r r r", SHR (794)},               /* srad ra,rs,rb */
  {MIR_RSHS, "r r r", SHR (792)},              /* sraw ra,rs,rb */
  {MIR_RSH, "r r Sh", "o31 p413 rs1 ra0 Sh"},  /* sradi ra,rs */
  {MIR_RSHS, "r r sh", "o31 O824 rs1 ra0 sh"}, /* srawi ra,rs */

  {MIR_URSH, "r r r", SHR (539)},                     /* srd ra,rs,rb */
  {MIR_URSHS, "r r r", SHR (536)},                    /* srw ra,rs,rb */
  {MIR_URSH, "r r Sh", "o30 ra0 rs1 Shr mbSh"},       /* rldicl ra,rs,64-sh,sh */
  {MIR_URSHS, "r r sh", "o21 ra0 rs1 shr mbsh me31"}, /* rlwinm ra,rs,32-sh,sh,31 */

// ??? nand nor
#define LOGR(s) "o31 O" #s "  ra0 rs1 rb2"
#define LOGU(s) "o" #s "  ra0 rs1 u"
#define LOGUS(s) "o" #s "  ra0 rs1 U"
  {MIR_AND, "r r r", LOGR (28)},   /* and ra,rs,rb */
  {MIR_AND, "r r u", LOGU (28)},   /* andi. ra,rs,u */
  {MIR_AND, "r r U", LOGUS (29)},  /* andis. ra,rs,U */
  {MIR_ANDS, "r r r", LOGR (28)},  /* and ra,rs,rb */
  {MIR_ANDS, "r r u", LOGU (28)},  /* andi. ra,rs,u */
  {MIR_ANDS, "r r U", LOGUS (29)}, /* andis. ra,rs,U */

  {MIR_OR, "r r r", LOGR (444)},  /* or ra,rs,rb */
  {MIR_OR, "r r u", LOGU (24)},   /* ori ra,rs,u */
  {MIR_OR, "r r U", LOGUS (25)},  /* oris ra,rs,U */
  {MIR_ORS, "r r r", LOGR (444)}, /* or ra,rs,rb */
  {MIR_ORS, "r r u", LOGU (24)},  /* ori ra,rs,u */
  {MIR_ORS, "r r U", LOGUS (25)}, /* oris ra,rs,U */

  {MIR_XOR, "r r r", LOGR (316)},  /* xor ra,rs,rb */
  {MIR_XOR, "r r u", LOGU (26)},   /* xori ra,rs,u */
  {MIR_XOR, "r r U", LOGUS (27)},  /* xoris ra,rs,U */
  {MIR_XORS, "r r r", LOGR (316)}, /* xor ra,rs,rb */
  {MIR_XORS, "r r u", LOGU (26)},  /* xori ra,rs,u */
  {MIR_XORS, "r r U", LOGUS (27)}, /* xoris ra,rs,U */

  /* std rt1,-16(r1); lfd f0,-16(r1); fcfids rt0,f0: */
  {MIR_I2F, "r r", "o62 rs1 mt; o50 ht32 mt; o59 O846 rt0 hb32"},
  /* std rt1,-16(r1); lfd f0,-16(r1); fcfid rt0,f0: */
  {MIR_I2D, "r r", "o62 rs1 mt; o50 ht32 mt; o63 O846 rt0 hb32"},
  /* std rt1,-16(r1); lfd f0,-16(r1); fcfidus rt0,f0: */
  {MIR_UI2F, "r r", "o62 rs1 mt; o50 ht32 mt; o59 O974 rt0 hb32"},
  /* std rt1,-16(r1); lfd f0,-16(r1); fcfid rt0,f0: */
  {MIR_UI2D, "r r", "o62 rs1 mt; o50 ht32 mt; o63 O974 rt0 hb32"},
  /* fctidz f0,rb; stfd f0,-16(r1);ld rt,-16(r1): */
  {MIR_F2I, "r r", "o63 O815 ht32 rb1; o54 hs32 mt; o58 rt0 mt"},
  /* fctidz f0,rb; stfd f0,-16(r1);ld rt,-16(r1): */
  {MIR_D2I, "r r", "o63 O815 ht32 rb1; o54 hs32 mt; o58 rt0 mt"},
  {MIR_F2D, "r r", "o63 O72 rt0 rb1"}, /* fmr rt,rb */
  {MIR_D2F, "r r", "o63 O12 rt0 rb1"}, /* frsp rt,rb */
                                       // i2ld, ui2ld, ld2i, f2ld, d2ld, ld2f, ld2d are builtins

  {MIR_CALL, "X h12 $", "o31 O467 rs1 sr9; o19 O528 BO20 BI0 LK1"}, /* mtctr r12; bcctrl */
  {MIR_CALL, "X r $",
   "o31 O444 ha12 rs1 rb1; o31 O467 rs1 sr9; o19 O528 BO20 BI0 LK1"}, /* mr r12,r; mtctr r; bcctrl
                                                                       */

  {MIR_RET, "$", "o19 O16 BO20 BI0"}, /* bclr */

  {MIR_JCALL, "X r $", "o31 O467 rs1 sr9; o19 O528 BO20 BI0"}, /* mtctr r; bcctr */
  {MIR_JRET, "r $", "o31 O467 rs0 sr9; o19 O528 BO20 BI0"},    /* mtctr r; bcctr */

/* subf r1,rt,r1; ldx r0,(r1,rt); std r0,0(r1);
   add rt,r1,PPC64_STACK_HEADER_SIZE+PARAM_AREA_SIZE: */
#define ALLOCA_END                             \
  "o31 O40 ht1 ra0 hb1; o31 O21 ht0 ha1 rb0; " \
  "o62 hs0 ha1; o14 rt0 ha1 ih"
  /* addi rt,ra,15;rldicr rt,rt,0,59; ... : */
  {MIR_ALLOCA, "r r", "o14 rt0 ra1 i15; o30 ra0 rs0 Sh0 Me59; " ALLOCA_END},
  /* mov rt,ia; ...: */
  {MIR_ALLOCA, "r ia", "o14 rt0 ha0 ia; " ALLOCA_END},

  {MIR_BSTART, "r", "o31 O444 ra0 hs1 hb1"}, /* or ra,r1,r1 */
  /* ld r0,0(r1);or r1,rs,rs; std r0,0(r1): */
  {MIR_BEND, "r", "o58 hs0 ha1;o31 O444 ha1 rs0 rb0; o62 hs0 ha1"},

  /* bl l4; mflr r0; rldicr r10,rt,3,60; add r10,r0,r10; ld r0,table-disp(r10); mtctr r0; bcctr;
     TableContent: */
  {MIR_SWITCH, "r $",
   "o18 l4 LK1; o31 O339 ht0 sr8; o30 ha10 rs0 Sh3 Me60; o31 O266 ht10 ha0 hb10; o58 ht0 ha10 T; "
   "o31 O467 hs0 sr9; o19 O528 BO20 BI0"},
};

static void target_get_early_clobbered_hard_regs (MIR_insn_t insn, MIR_reg_t *hr1, MIR_reg_t *hr2) {
  MIR_insn_code_t code = insn->code;

  *hr1 = *hr2 = MIR_NON_VAR;
  if (code == MIR_MOD || code == MIR_MODS || code == MIR_UMOD || code == MIR_UMODS) {
    *hr1 = R10_HARD_REG;
  } else if (code == MIR_I2F || code == MIR_I2D || code == MIR_UI2F || code == MIR_UI2D
             || code == MIR_F2I || code == MIR_D2I) {
    *hr1 = F0_HARD_REG;
  } else if (code == MIR_LDMOV) { /* if mem base reg is R0 */
    *hr1 = R11_HARD_REG; /* don't use arg regs as ldmov can be used in param passing part */
  } else if (code == MIR_CALL || code == MIR_INLINE) {
    *hr1 = R12_HARD_REG;
  } else if (code == MIR_SWITCH) {
    *hr1 = R10_HARD_REG;
  }
}

static int pattern_index_cmp (const void *a1, const void *a2) {
  int i1 = *(const int *) a1, i2 = *(const int *) a2;
  int c1 = (int) patterns[i1].code, c2 = (int) patterns[i2].code;

  return c1 != c2 ? c1 - c2 : (long) i1 - (long) i2;
}

static void patterns_init (gen_ctx_t gen_ctx) {
  MIR_alloc_t alloc = gen_alloc (gen_ctx);
  int i, ind, n = sizeof (patterns) / sizeof (struct pattern);
  MIR_insn_code_t prev_code, code;
  insn_pattern_info_t *info_addr;
  insn_pattern_info_t pinfo = {0, 0};

  VARR_CREATE (int, pattern_indexes, alloc, 0);
  for (i = 0; i < n; i++) VARR_PUSH (int, pattern_indexes, i);
  qsort (VARR_ADDR (int, pattern_indexes), n, sizeof (int), pattern_index_cmp);
  VARR_CREATE (insn_pattern_info_t, insn_pattern_info, alloc, 0);
  for (i = 0; i < MIR_INSN_BOUND; i++) VARR_PUSH (insn_pattern_info_t, insn_pattern_info, pinfo);
  info_addr = VARR_ADDR (insn_pattern_info_t, insn_pattern_info);
  for (prev_code = MIR_INSN_BOUND, i = 0; i < n; i++) {
    ind = VARR_GET (int, pattern_indexes, i);
    if ((code = patterns[ind].code) != prev_code) {
      if (i != 0) info_addr[prev_code].num = i - info_addr[prev_code].start;
      info_addr[code].start = i;
      prev_code = code;
    }
  }
  assert (prev_code != MIR_INSN_BOUND);
  info_addr[prev_code].num = n - info_addr[prev_code].start;
}

static int int16_p (int64_t i) { return -(1 << 15) <= i && i < (1 << 15); }
static int uint16_p (uint64_t u) { return !(u >> 16); }
static int int16_shifted_p (int64_t i) { return (i & 0xffff) == 0 && int16_p (i >> 16); }
static int uint16_shifted_p (uint64_t u) { return (u & 0xffff) == 0 && uint16_p (u >> 16); }
static int uint31_p (uint64_t u) { return !(u >> 31); }
static int uint47_p (uint64_t u) { return !(u >> 47); }
static int uint32_p (uint64_t u) { return !(u >> 32); }
static int uint6_p (uint64_t u) { return !(u >> 6); }
static int uint5_p (uint64_t u) { return !(u >> 5); }
static int negative32_p (uint64_t u, uint64_t *n) {
  if (((u >> 31) & 1) == 0) return FALSE;
  /* high 32-bit part pattern: 0*1*, n contains number of ones. */
  for (u >>= 32, *n = 0; u & 1; u >>= 1, (*n)++)
    ;
  return u == 0;
}

static int pattern_match_p (gen_ctx_t gen_ctx, const struct pattern *pat, MIR_insn_t insn,
                            int use_short_label_p) {
  MIR_context_t ctx = gen_ctx->ctx;
  size_t nop, nops = MIR_insn_nops (ctx, insn);
  const char *p;
  char ch, start_ch;
  MIR_op_t op;
  MIR_reg_t hr;

  for (nop = 0, p = pat->pattern; *p != 0; p++, nop++) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '$') return TRUE;
    if (MIR_call_code_p (insn->code) && nop >= nops) return FALSE;
    gen_assert (nop < nops);
    op = insn->ops[nop];
    switch (start_ch = *p) {
    case 'X': break;
    case 'r':
      if (op.mode != MIR_OP_VAR || op.u.var == LR_HARD_REG) return FALSE;
      break;
    case 'R':
      if (op.mode != MIR_OP_VAR || op.u.var == R0_HARD_REG || op.u.var == LR_HARD_REG) return FALSE;
      break;
    case 'h':
      if (op.mode != MIR_OP_VAR) return FALSE;
      ch = *++p;
      gen_assert ('0' <= ch && ch <= '9');
      hr = ch - '0';
      ch = *++p;
      if ('0' <= ch && ch <= '9')
        hr = hr * 10 + ch - '0';
      else
        --p;
      gen_assert (hr <= MAX_HARD_REG);
      if (op.u.var != hr) return FALSE;
      break;
    case 'm':
    case 'M': {
      MIR_type_t type, type2, type3 = MIR_T_BOUND;
      int ds_p = FALSE, l_p = FALSE, br0_p = FALSE, u_p = TRUE, s_p = TRUE;

      if (op.mode != MIR_OP_VAR_MEM) return FALSE;
      ch = *++p;
      switch (ch) {
      case 'f':
        type = MIR_T_F;
        type2 = MIR_T_BOUND;
        break;
      case 'd':
        ch = *++p;
        if (ch == 's') {
          ds_p = s_p = TRUE;
          type = start_ch == 'M' ? MIR_T_I64 : MIR_T_I32;
          type2 = start_ch == 'M' ? MIR_T_U64 : MIR_T_BOUND;
#if MIR_PTR32
          if (start_ch == 'm') type3 = MIR_T_P;
#else
          if (start_ch == 'M') type3 = MIR_T_P;
#endif
        } else {
          p--;
          type = MIR_T_D;
          type2 = MIR_T_BOUND;
        }
        break;
      case 'l':
        ch = *++p;
        gen_assert (ch == 'd' && start_ch != 'M');
        ch = *++p;
        if (ch != '0')
          p--;
        else
          br0_p = TRUE;
        l_p = TRUE;
        type = MIR_T_LD;
        type2 = MIR_T_BOUND;
        break;
      case 'u':
      case 's':
        u_p = ch == 'u';
        s_p = ch == 's';
        ch = *++p;
        /* fall through */
      default:
        gen_assert ('0' <= ch && ch <= '3');
        if (ch == '0') {
          type = u_p ? MIR_T_U8 : MIR_T_I8;
          type2 = u_p && s_p ? MIR_T_I8 : MIR_T_BOUND;
        } else if (ch == '1') {
          type = u_p ? MIR_T_U16 : MIR_T_I16;
          type2 = u_p && s_p ? MIR_T_I16 : MIR_T_BOUND;
        } else if (ch == '2') {
          type = u_p ? MIR_T_U32 : MIR_T_I32;
          type2 = u_p && s_p ? MIR_T_I32 : MIR_T_BOUND;
#if MIR_PTR32
          if (u_p) type3 = MIR_T_P;
#endif
        } else {
          type = u_p ? MIR_T_U64 : MIR_T_I64;
          type2 = u_p && s_p ? MIR_T_I64 : MIR_T_BOUND;
#if MIR_PTR64
          type3 = MIR_T_P;
#endif
        }
      }
      if (op.u.var_mem.type != type && op.u.var_mem.type != type2 && op.u.var_mem.type != type3)
        return FALSE;
      if (ds_p
          && (op.u.var_mem.index != MIR_NON_VAR || op.u.var_mem.base == R0_HARD_REG
              || op.u.var_mem.disp % 4 != 0 || !int16_p (op.u.var_mem.disp)))
        return FALSE;
      if (!ds_p && start_ch == 'm'
          && (op.u.var_mem.index != MIR_NON_VAR || (!br0_p && op.u.var_mem.base == R0_HARD_REG)
              || (br0_p && op.u.var_mem.base != R0_HARD_REG) || !int16_p (op.u.var_mem.disp)
              || (l_p && !int16_p (op.u.var_mem.disp + 8))))
        return FALSE;
      if (!ds_p && start_ch == 'M'
          && (op.u.var_mem.disp != 0
              || (op.u.var_mem.index != MIR_NON_VAR && op.u.var_mem.scale != 1)
              || (op.u.var_mem.base == R0_HARD_REG && op.u.var_mem.index != MIR_NON_VAR)))
        return FALSE;
      break;
    }
    case 'i':
      if (op.mode != MIR_OP_INT && op.mode != MIR_OP_UINT) return FALSE;
      ch = *++p;
      if (ch == 'a') {
        if (!int16_p ((op.u.i + 15) / 16 * 16)) return FALSE;
      } else {
        p--;
        if (!int16_p (op.u.i)) return FALSE;
      }
      break;
    case 'u':
      if ((op.mode != MIR_OP_INT && op.mode != MIR_OP_UINT) || !uint16_p (op.u.u)) return FALSE;
      break;
    case 'I':
      if ((op.mode != MIR_OP_INT && op.mode != MIR_OP_UINT) || !int16_shifted_p (op.u.i))
        return FALSE;
      break;
    case 'U':
      if ((op.mode != MIR_OP_INT && op.mode != MIR_OP_UINT) || !uint16_shifted_p (op.u.u))
        return FALSE;
      break;
    case 'x':
    case 'z':
    case 'Z': {
      uint64_t v, n;

      if (op.mode != MIR_OP_INT && op.mode != MIR_OP_UINT && op.mode != MIR_OP_REF) return FALSE;
      if (op.mode != MIR_OP_REF) {
        v = op.u.u;
      } else if (op.u.ref->item_type == MIR_data_item && op.u.ref->u.data->name != NULL
                 && _MIR_reserved_ref_name_p (ctx, op.u.ref->u.data->name)) {
        v = (uint64_t) op.u.ref->u.data->u.els;
      } else {
        v = (uint64_t) op.u.ref->addr;
      }
      if (start_ch == 'x') {
        if (!negative32_p (v, &n)) return FALSE;
      } else {
        ch = *++p;
        if (ch == 's') {
          if (start_ch == 'z' ? !uint31_p (v) : !uint47_p (op.u.u)) return FALSE;
        } else {
          p--;
          if (start_ch == 'z' && !uint32_p (v)) return FALSE;
        }
      }
      break;
    }
    case 's':
    case 'S':
      ch = *++p;
      gen_assert (ch == 'h');
      if (op.mode != MIR_OP_INT && op.mode != MIR_OP_UINT) return FALSE;
      if ((start_ch == 's' && !uint5_p (op.u.u)) || (start_ch == 'S' && !uint6_p (op.u.u)))
        return FALSE;
      break;
    case 'l':
      if (op.mode != MIR_OP_LABEL || !use_short_label_p) return FALSE;
      break;
    case 'L':
      if (op.mode != MIR_OP_LABEL && op.mode != MIR_OP_REF) return FALSE;
      break;
    case 'W':
      if (op.mode != MIR_OP_LABEL) return FALSE;
      break;
    default: gen_assert (FALSE);
    }
  }
  gen_assert (nop == nops);
  return TRUE;
}

static const char *find_insn_pattern_replacement (gen_ctx_t gen_ctx, MIR_insn_t insn,
                                                  int use_short_label_p) {
  int i;
  const struct pattern *pat;
  insn_pattern_info_t info = VARR_GET (insn_pattern_info_t, insn_pattern_info, insn->code);

  for (i = 0; i < info.num; i++) {
    pat = &patterns[VARR_GET (int, pattern_indexes, info.start + i)];
    if (pattern_match_p (gen_ctx, pat, insn, use_short_label_p)) return pat->replacement;
  }
  return NULL;
}

static void patterns_finish (gen_ctx_t gen_ctx) {
  VARR_DESTROY (int, pattern_indexes);
  VARR_DESTROY (insn_pattern_info_t, insn_pattern_info);
}

static int dec_value (int ch) { return '0' <= ch && ch <= '9' ? ch - '0' : -1; }

static uint64_t read_dec (const char **ptr) {
  int v;
  const char *p;
  uint64_t res = 0;

  for (p = *ptr; (v = dec_value (*p)) >= 0; p++) {
    gen_assert ((res >> 60) == 0);
    res = res * 10 + v;
  }
  gen_assert (p != *ptr);
  *ptr = p - 1;
  return res;
}

static uint32_t check_and_set_mask (uint32_t result_mask, uint32_t mask) {
  gen_assert ((result_mask & mask) == 0);
  return result_mask | mask;
}

static void put_uint32 (struct gen_ctx *gen_ctx, uint32_t v) {
  VARR_PUSH_ARR (uint8_t, result_code, (uint8_t *) &v, sizeof (v)); /* reserve */
  /* write with the right endianess: */
  ((uint32_t *) (VARR_ADDR (uint8_t, result_code) + VARR_LENGTH (uint8_t, result_code)))[-1] = v;
}

static void put_uint64 (struct gen_ctx *gen_ctx, uint64_t v) {
  VARR_PUSH_ARR (uint8_t, result_code, (uint8_t *) &v, sizeof (v)); /* reserve */
  ((uint64_t *) (VARR_ADDR (uint8_t, result_code) + VARR_LENGTH (uint8_t, result_code)))[-1] = v;
}

static void set_int64 (uint8_t *addr, int64_t v) { *(int64_t *) addr = v; }

static int64_t get_int64 (uint8_t *addr) { return *(int64_t *) addr; }

static void out_insn (gen_ctx_t gen_ctx, MIR_insn_t insn, const char *replacement,
                      void **jump_addrs) {
  MIR_context_t ctx = gen_ctx->ctx;
  size_t nops = MIR_insn_nops (ctx, insn);
  size_t offset;
  const char *p, *insn_str;
  label_ref_t lr;
  int switch_table_addr_insn_start = -1;
  uint32_t nop_binsn = 24 << (32 - 6); /* ori 0,0,0 */

  if (insn->code == MIR_ALLOCA
      && (insn->ops[1].mode == MIR_OP_INT || insn->ops[1].mode == MIR_OP_UINT))
    insn->ops[1].u.u = (insn->ops[1].u.u + 15) & -16;
  for (insn_str = replacement;; insn_str = p + 1) {
    MIR_op_t op;
    char ch, ch2, start_ch;
    uint32_t binsn = 0;
    int opcode = -1, opcode2 = -1, opcode3 = -1, opcode4 = -1, rt = -1, rs = -1, ra = -1, rb = -1,
        rc = -1, spreg = -1, sh = -1, Sh = -1;
    int disp = -1, disp4 = -1, mb = -1, me = -1, Mb = -1, Me = -1, bf = -1, BO = -1, BI = -1,
        imm = -1, LK = -1;
    int d = -1, lab_off = -1, lb = -1, label_ref_num = -1, n;
    uint32_t binsn_mask = 0;
    int switch_table_addr_p = FALSE;

    for (p = insn_str; (ch = *p) != '\0' && ch != ';'; p++) {
      if ((ch = *p) == 0 || ch == ';') break;
      switch ((start_ch = ch = *p)) {
      case ' ':
      case '\t': break;
      case 'o':
        ch2 = *++p;
        gen_assert (dec_value (ch2) >= 0 && opcode < 0);
        opcode = read_dec (&p);
        break;
      case 'O':
        ch2 = *++p;
        gen_assert (dec_value (ch2) >= 0 && opcode2 < 0);
        opcode2 = read_dec (&p);
        break;
      case 'p':
        ch2 = *++p;
        gen_assert (dec_value (ch2) >= 0 && opcode3 < 0);
        opcode3 = read_dec (&p);
        break;
      case 'P':
        ch2 = *++p;
        gen_assert (dec_value (ch2) >= 0 && opcode4 < 0);
        opcode4 = read_dec (&p);
        break;
      case 'r':
      case 'n':
      case 'R':
      case 'h': {
        int reg;

        ch2 = *++p;
        gen_assert (ch2 == 't' || ch2 == 's' || ch2 == 'a' || ch2 == 'b' || ch2 == 'c');
        gen_assert (start_ch != 'R' || ch2 == 'a');
        ch = *++p;
        if (start_ch == 'h') {
          reg = read_dec (&p);
        } else {
          gen_assert ('0' <= ch && ch <= '2' && ch - '0' < (int) insn->nops);
          op = insn->ops[ch - '0'];
          gen_assert (op.mode == MIR_OP_VAR);
          reg = op.u.var + (start_ch == 'n' ? 1 : 0);
        }
        if (reg > R31_HARD_REG) reg -= F0_HARD_REG;
        gen_assert (reg <= 31);
        if (ch2 == 't') {
          gen_assert (rt < 0);
          rt = reg;
        } else if (ch2 == 's') {
          gen_assert (rs < 0);
          rs = reg;
        } else if (ch2 == 'a') {
          gen_assert (ra < 0);
          ra = reg;
        } else if (ch2 == 'b') {
          gen_assert (rb < 0);
          rb = reg;
        } else {
          gen_assert (rc < 0);
          rc = reg;
        }
        break;
      }
      case 's':
        ch2 = *++p;
        if (ch2 == 'r') {
          ch2 = *++p;
          gen_assert (dec_value (ch2) >= 0 && spreg < 0);
          spreg = read_dec (&p);
        } else if (ch2 == 'h') {
          op = insn->ops[2];
          ch2 = *++p;
          gen_assert (sh < 0);
          if (dec_value (ch2) >= 0) {
            sh = read_dec (&p);
          } else if (ch2 == 'r') {
            gen_assert (op.mode == MIR_OP_INT || op.mode == MIR_OP_UINT);
            sh = 32 - op.u.u;
          } else {
            --p;
            gen_assert (op.mode == MIR_OP_INT || op.mode == MIR_OP_UINT);
            sh = op.u.u;
          }
        }
        break;
      case 'S':
        ch2 = *++p;
        gen_assert (ch2 == 'h' && Sh < 0);
        ch2 = *++p;
        if (dec_value (ch2) >= 0) {
          Sh = read_dec (&p);
        } else if (ch2 == 'r') {
          op = insn->ops[2];
          gen_assert (op.mode == MIR_OP_INT || op.mode == MIR_OP_UINT);
          Sh = 64 - op.u.u;
        } else {
          --p;
          op = insn->ops[2];
          gen_assert (op.mode == MIR_OP_INT || op.mode == MIR_OP_UINT);
          Sh = op.u.u;
        }
        break;
      case 'M': {
        int b_p;

        ch2 = *++p;
        if (ch2 == '9') {
          ch2 = *++p;
          gen_assert (ch2 == '1');
          ch2 = *++p;
          gen_assert (ch2 == '0' && ra < 0 && rb < 0);
          ra = R9_HARD_REG;
          rb = R10_HARD_REG;
        } else if ((b_p = ch2 == 'b') || ch2 == 'e') {
          ch2 = *++p;
          gen_assert (dec_value (ch2) >= 0 && Mb < 0 && Me < 0);
          if (b_p) {
            Mb = read_dec (&p);
            Mb = ((Mb & 0x1f) << 1) | ((Mb >> 5) & 1);
          } else {
            Me = read_dec (&p);
            Me = ((Me & 0x1f) << 1) | ((Me >> 5) & 1);
          }
        } else {
          op = insn->ops[0].mode == MIR_OP_VAR_MEM ? insn->ops[0] : insn->ops[1];
          gen_assert (op.mode == MIR_OP_VAR_MEM);
          if (ch2 == 'd') {
            ch2 = *++p;
            gen_assert (ch2 == 's' && ra < 0 && disp4 < 0);
            ra = (int) op.u.var_mem.base;
            if (op.u.var_mem.base == MIR_NON_VAR) ra = R0_HARD_REG;
            disp4 = op.u.var_mem.disp & 0xffff;
            gen_assert ((disp4 & 0x3) == 0);
          } else {
            --p;
            gen_assert (ra < 0 && rb < 0);
            ra = (int) op.u.var_mem.base;
            rb = (int) op.u.var_mem.index;
            if (op.u.var_mem.index == MIR_NON_VAR) {
              rb = ra;
              ra = R0_HARD_REG;
            } else if (op.u.var_mem.base == MIR_NON_VAR) {
              ra = R0_HARD_REG;
            } else if (ra == R0_HARD_REG) {
              ra = rb;
              ra = R0_HARD_REG;
            }
          }
        }
        break;
      }
      case 'm': {
        int b_p, single_p;

        ch2 = *++p;
        if (ch2 == 't') {
          gen_assert (ra < 0 && disp < 0);
          disp = (-16) & 0xffff;
          ra = R1_HARD_REG;
        } else if ((b_p = ch2 == 'b') || ch2 == 'e') {
          ch2 = *++p;
          if (dec_value (ch2) >= 0) {
            if (b_p) {
              gen_assert (mb < 0);
              mb = read_dec (&p);
            } else {
              gen_assert (me < 0);
              me = read_dec (&p);
            }
          } else {
            single_p = ch2 == 'S';
            gen_assert (ch2 == 's' || ch2 == 'S');
            ch2 = *++p;
            gen_assert (ch2 == 'h');
            op = insn->ops[2];
            gen_assert (op.mode == MIR_OP_INT || op.mode == MIR_OP_UINT);
            if (single_p) {
              gen_assert (Mb < 0 && Me < 0);
              if (b_p) {
                Mb = op.u.i;
                Mb = ((Mb & 0x1f) << 1) | ((Mb >> 5) & 1);
              } else {
                Me = 63 - op.u.i;
                Me = ((Me & 0x1f) << 1) | ((Me >> 5) & 1);
              }
            } else if (b_p) {
              gen_assert (mb < 0);
              mb = op.u.i;
            } else {
              gen_assert (me < 0);
              me = 31 - op.u.i;
            }
          }
        } else {
          op = insn->ops[0].mode == MIR_OP_VAR_MEM ? insn->ops[0] : insn->ops[1];
          gen_assert (op.mode == MIR_OP_VAR_MEM);
          if (ch2 == 'd') {
            ch2 = *++p;
            gen_assert (ch2 == 's' && ra < 0 && disp4 < 0);
            ra = (int) op.u.var_mem.base;
            if (op.u.var_mem.base == MIR_NON_VAR) ra = R0_HARD_REG;
            disp4 = op.u.var_mem.disp & 0xffff;
            gen_assert ((disp4 & 0x3) == 0);
          } else {
            if (ch2 != 'n') --p;
            gen_assert (ra < 0 && disp < 0);
            ra = (int) op.u.var_mem.base;
            if (op.u.var_mem.base == MIR_NON_VAR) ra = R0_HARD_REG;
            disp = (op.u.var_mem.disp + (ch2 != 'n' ? 0 : 8)) & 0xffff;
          }
        }
        break;
      }
      case 'd':
        ch2 = *++p;
        gen_assert (d < 0 && dec_value (ch2) >= 0);
        d = read_dec (&p);
        break;
      case 'i':
        ch2 = *++p;
        if (ch2 == 'a') {
          op = insn->ops[nops - 1];
          gen_assert (op.mode == MIR_OP_INT || op.mode == MIR_OP_UINT);
          gen_assert (imm < 0);
          imm = (op.u.i + 15) / 16 * 16;
          break;
        } else if (ch2 == 'h') {
          gen_assert (imm < 0);
          imm = PPC64_STACK_HEADER_SIZE + param_save_area_size;
          break;
        } else if (dec_value (ch2) >= 0) {
          gen_assert (imm < 0);
          imm = read_dec (&p);
          break;
        }
        p--;
        /* fall through */
      case 'u':
      case 'I':
      case 'U':
        op = insn->ops[nops - 1];
        gen_assert (op.mode == MIR_OP_INT || op.mode == MIR_OP_UINT);
        gen_assert (imm < 0);
        imm = (start_ch == 'i' || start_ch == 'u' ? op.u.u : op.u.u >> 16) & 0xffff;
        break;
      case 'x':
      case 'z': {
        int ok_p;
        uint64_t v;

        op = insn->ops[nops - 1];
        gen_assert (op.mode == MIR_OP_INT || op.mode == MIR_OP_UINT || op.mode == MIR_OP_REF);
        if (op.mode != MIR_OP_REF) {
          v = op.u.u;
        } else if (op.u.ref->item_type == MIR_data_item && op.u.ref->u.data->name != NULL
                   && _MIR_reserved_ref_name_p (ctx, op.u.ref->u.data->name)) {
          v = (uint64_t) op.u.ref->u.data->u.els;
        } else {
          v = (uint64_t) op.u.ref->addr;
        }
        if (start_ch == 'x') {
          uint64_t num;
          ok_p = negative32_p (v, &num);
          num = 32 - num;
          gen_assert (Mb < 0 && ok_p);
          Mb = ((num & 0x1f) << 1) | ((num >> 5) & 1);
        } else {
          gen_assert (imm < 0);
          n = dec_value (*++p);
          gen_assert (n >= 0 && n <= 3);
          imm = (v >> (3 - n) * 16) & 0xffff;
        }
        break;
      }
      case 'b':
        ch2 = *++p;
        gen_assert (ch2 == 'f');
        ch2 = *++p;
        gen_assert (dec_value (ch2) >= 0);
        gen_assert (bf < 0);
        bf = read_dec (&p);
        break;
      case 'B': {
        int o_p;

        ch2 = *++p;
        gen_assert (ch2 == 'O' || ch2 == 'I');
        o_p = ch2 == 'O';
        ch2 = *++p;
        gen_assert (dec_value (ch2) >= 0);
        if (o_p) {
          gen_assert (BO < 0);
          BO = read_dec (&p);
        } else {
          gen_assert (BI < 0);
          BI = read_dec (&p);
        }
        break;
      }
      case 'l': {
        ch2 = *++p;
        if ((n = dec_value (ch2)) >= 0) {
          gen_assert (lab_off < 0 && (n & 0x3) == 0);
          lab_off = n;
        } else {
          --p;
          gen_assert (insn->code != MIR_CALL);
          op = insn->ops[0];
          gen_assert (op.mode == MIR_OP_LABEL);
          lr.abs_addr_p = FALSE;
          lr.branch_type = BRCOND;
          lr.label_val_disp = 0;
          if (jump_addrs == NULL)
            lr.u.label = op.u.label;
          else
            lr.u.jump_addr = jump_addrs[0];
          label_ref_num = VARR_LENGTH (label_ref_t, label_refs);
          VARR_PUSH (label_ref_t, label_refs, lr);
        }
        break;
      }
      case 'L': {
        ch2 = *++p;
        if (ch2 == 'K') {
          ch2 = *++p;
          gen_assert (LK < 0 && dec_value (ch2) >= 0);
          LK = read_dec (&p);
          gen_assert (LK <= 1);
        } else if ((n = dec_value (ch2)) >= 0) {
          gen_assert (lb < 0);
          lb = n;
        } else {
          --p;
          op = insn->ops[insn->code != MIR_CALL ? 0 : 1];
          gen_assert (op.mode == MIR_OP_LABEL);
          lr.abs_addr_p = FALSE;
          lr.branch_type = JUMP;
          lr.label_val_disp = 0;
          if (jump_addrs == NULL)
            lr.u.label = op.u.label;
          else
            lr.u.jump_addr = jump_addrs[0];
          label_ref_num = VARR_LENGTH (label_ref_t, label_refs);
          VARR_PUSH (label_ref_t, label_refs, lr);
        }
        break;
      }
      case 'W': {
        op = insn->ops[1];
        gen_assert (insn->code == MIR_LADDR && op.mode == MIR_OP_LABEL);
        lr.abs_addr_p = FALSE;
        lr.branch_type = LADDR;
        lr.label_val_disp = 0;
        if (jump_addrs == NULL)
          lr.u.label = op.u.label;
        else
          lr.u.jump_addr = jump_addrs[0];
        label_ref_num = VARR_LENGTH (label_ref_t, label_refs);
        VARR_PUSH (label_ref_t, label_refs, lr);
        break;
      }
      case 'a':
        gen_assert (imm < 0);
        ch2 = *++p;
        gen_assert (ch2 == 't' || ch2 == 'a');
        imm = ch2 == 't' ? PPC64_TOC_OFFSET : 15 + PPC64_STACK_HEADER_SIZE + param_save_area_size;
        break;
      case 'T':
        gen_assert (!switch_table_addr_p && switch_table_addr_insn_start < 0);
        switch_table_addr_p = TRUE;
        break;
      default: gen_assert (FALSE);
      }
    }

    if (opcode >= 0) {
      gen_assert (opcode < 64);
      binsn |= opcode << (32 - 6);
      binsn_mask = check_and_set_mask (binsn_mask, 0x3f << (32 - 6));
    }
    if (opcode2 >= 0) {
      gen_assert (opcode2 < (1 << 10));
      binsn |= opcode2 << 1;
      binsn_mask = check_and_set_mask (binsn_mask, 0x3ff << 1);
    }
    if (opcode3 >= 0) {
      gen_assert (opcode3 < (1 << 9));
      binsn |= opcode3 << 2;
      binsn_mask = check_and_set_mask (binsn_mask, 0x1ff << 2);
    }
    if (opcode4 >= 0) {
      gen_assert (opcode4 < (1 << 5));
      binsn |= opcode4 << 1;
      binsn_mask = check_and_set_mask (binsn_mask, 0x1f << 1);
    }
    if (rt >= 0) {
      gen_assert (rt < 32);
      binsn |= rt << (32 - 11);
      binsn_mask = check_and_set_mask (binsn_mask, 0x1f << (32 - 11));
    }
    if (rs >= 0) {
      gen_assert (rs < 32);
      binsn |= rs << (32 - 11);
      binsn_mask = check_and_set_mask (binsn_mask, 0x1f << (32 - 11));
    }
    if (ra >= 0) {
      gen_assert (ra < 32);
      binsn |= ra << (32 - 16);
      binsn_mask = check_and_set_mask (binsn_mask, 0x1f << (32 - 16));
    }
    if (rb >= 0) {
      gen_assert (rb < 32);
      binsn |= rb << (32 - 21);
      binsn_mask = check_and_set_mask (binsn_mask, 0x1f << (32 - 21));
    }
    if (rc >= 0) {
      gen_assert (rc < 32);
      binsn |= rc << (32 - 26);
      binsn_mask = check_and_set_mask (binsn_mask, 0x1f << (32 - 26));
    }
    if (spreg >= 0) {
      gen_assert (spreg < (1 << 5));
      binsn |= spreg << 16;
      binsn_mask = check_and_set_mask (binsn_mask, 0x3ff << (32 - 21));
    }
    if (disp >= 0) {
      gen_assert (disp < (1 << 16));
      binsn |= disp;
      binsn_mask = check_and_set_mask (binsn_mask, 0xffff);
    }
    if (disp4 >= 0) {
      gen_assert (disp4 < (1 << 16) && (disp4 & 0x3) == 0);
      binsn |= disp4;
      binsn_mask = check_and_set_mask (binsn_mask, 0xfffc);
    }
    if (d >= 0) {
      gen_assert (d < (1 << 2));
      binsn |= d;
      binsn_mask = check_and_set_mask (binsn_mask, 0x3);
    }
    if (Sh >= 0) {
      gen_assert (Sh < (1 << 6));
      binsn |= ((Sh & 0x1f) << (32 - 21));
      binsn |= (Sh >> 4) & 0x2;
      binsn_mask = check_and_set_mask (binsn_mask, (0x1f << (32 - 21)) | 0x2);
    }
    if (sh >= 0) {
      gen_assert (sh < (1 << 5));
      binsn |= sh << (32 - 21);
      binsn_mask = check_and_set_mask (binsn_mask, 0x1f << (32 - 21));
    }
    if (mb >= 0) {
      gen_assert (mb < (1 << 5));
      binsn |= (mb & 0x1f) << 6;
      binsn_mask = check_and_set_mask (binsn_mask, (0x1f << 6));
    }
    if (me >= 0) {
      gen_assert (me < (1 << 5));
      binsn |= (me & 0x1f) << 1;
      binsn_mask = check_and_set_mask (binsn_mask, (0x1f << 1));
    }
    if (Mb >= 0) {
      gen_assert (Mb < (1 << 6));
      binsn |= (Mb & 0x3f) << (32 - 27);
      binsn_mask = check_and_set_mask (binsn_mask, (0x3f << (32 - 27)));
    }
    if (Me >= 0) {
      gen_assert (Me < (1 << 6));
      binsn |= (Me & 0x3f) << (32 - 27);
      binsn |= 1 << 2;
      binsn_mask = check_and_set_mask (binsn_mask, (0x3f << (32 - 27)) | (1 << 2));
    }
    if (imm >= 0) {
      gen_assert (imm < (1 << 16));
      binsn |= imm;
      binsn_mask = check_and_set_mask (binsn_mask, 0xffff);
    }
    if (lab_off >= 0) {
      gen_assert (lab_off < (1 << 16) && (lab_off & 0x3) == 0);
      binsn |= lab_off;
      binsn_mask = check_and_set_mask (binsn_mask, 0xfffc);
    }
    if (bf >= 0) {
      gen_assert (bf < 8);
      binsn |= bf << (32 - 9);
      binsn_mask = check_and_set_mask (binsn_mask, 0x7 << (32 - 9));
    }
    if (BO >= 0) {
      gen_assert (BO < 32);
      binsn |= BO << 21;
      binsn_mask = check_and_set_mask (binsn_mask, 0x1f << 21);
    }
    if (BI >= 0) {
      gen_assert (BI < 32);
      binsn |= BI << 16;
      binsn_mask = check_and_set_mask (binsn_mask, 0x1f << 16);
    }
    if (LK >= 0) {
      gen_assert (LK < 2);
      binsn |= LK;
      binsn_mask = check_and_set_mask (binsn_mask, 0x1);
    }
    if (lb >= 0) {
      gen_assert (lb < 2);
      binsn |= lb << (32 - 11);
      binsn_mask = check_and_set_mask (binsn_mask, 1 << (32 - 11));
    }
    if (label_ref_num >= 0) VARR_ADDR (label_ref_t, label_refs)
    [label_ref_num].label_val_disp = VARR_LENGTH (uint8_t, result_code);

    if (switch_table_addr_p) switch_table_addr_insn_start = VARR_LENGTH (uint8_t, result_code);
    put_uint32 (gen_ctx, binsn); /* output the machine insn */

    if (*p == 0) break;
  }

  if (switch_table_addr_insn_start < 0) return;
  if (VARR_LENGTH (uint8_t, result_code) % 8 == 4) put_uint32 (gen_ctx, nop_binsn);
  /* pc offset of T plus 3 insns after T: see switch */
  offset = (VARR_LENGTH (uint8_t, result_code) - switch_table_addr_insn_start) + 12;
  gen_assert ((offset & 0x3) == 0);
  *(uint32_t *) (VARR_ADDR (uint8_t, result_code) + switch_table_addr_insn_start) |= offset;
  gen_assert (insn->code == MIR_SWITCH);
  for (size_t i = 1; i < insn->nops; i++) {
    gen_assert (insn->ops[i].mode == MIR_OP_LABEL);
    lr.abs_addr_p = TRUE;
    lr.branch_type = BCTR; /* the value does not matter */
    lr.label_val_disp = VARR_LENGTH (uint8_t, result_code);
    if (jump_addrs == NULL)
      lr.u.label = insn->ops[i].u.label;
    else
      lr.u.jump_addr = jump_addrs[i - 1];
    VARR_PUSH (label_ref_t, label_refs, lr);
    put_uint64 (gen_ctx, 0); /* reserve mem for label address */
  }
}

static int target_memory_ok_p (gen_ctx_t gen_ctx, MIR_op_t *op_ref) {
  MIR_context_t ctx = gen_ctx->ctx;
  if (op_ref->mode != MIR_OP_VAR_MEM) return FALSE;
  if (op_ref->u.var_mem.index == MIR_NON_VAR && int16_p (op_ref->u.var_mem.disp)) return TRUE;
  size_t size = _MIR_type_size (ctx, op_ref->u.var_mem.type);
  if (op_ref->u.var_mem.index != MIR_NON_VAR && op_ref->u.var_mem.disp == 0
      && op_ref->u.var_mem.scale == size)
    return TRUE;
  if (op_ref->u.var_mem.index == MIR_NON_VAR && op_ref->u.var_mem.disp % 4 == 0
      && (size == 4 || size == 8) && int16_p (op_ref->u.var_mem.disp))
    return TRUE;
  return FALSE;
}

static int target_insn_ok_p (gen_ctx_t gen_ctx, MIR_insn_t insn) {
  return find_insn_pattern_replacement (gen_ctx, insn, TRUE) != NULL;
}

static void target_split_insns (gen_ctx_t gen_ctx) {
  for (MIR_insn_t insn = DLIST_HEAD (MIR_insn_t, curr_func_item->u.func->insns); insn != NULL;
       insn = DLIST_NEXT (MIR_insn_t, insn)) {
    MIR_insn_code_t code = insn->code;

    if ((code != MIR_RSH && code != MIR_LSH && code != MIR_URSH && code != MIR_RSHS
         && code != MIR_LSHS && code != MIR_URSHS)
        || (insn->ops[2].mode != MIR_OP_INT && insn->ops[2].mode != MIR_OP_UINT))
      continue;
    if (insn->ops[2].u.i == 0) {
      gen_mov (gen_ctx, insn, MIR_MOV, insn->ops[0], insn->ops[1]);
      MIR_insn_t old_insn = insn;
      insn = DLIST_PREV (MIR_insn_t, insn);
      gen_delete_insn (gen_ctx, old_insn);
    } else {
      if (insn->ops[2].mode == MIR_OP_INT && insn->ops[2].u.i < 0) {
        switch (code) {
        case MIR_RSH: insn->code = MIR_LSH; break;
        case MIR_URSH: insn->code = MIR_LSH; break;
        case MIR_LSH: insn->code = MIR_RSH; break;
        case MIR_RSHS: insn->code = MIR_LSHS; break;
        case MIR_URSHS: insn->code = MIR_LSHS; break;
        case MIR_LSHS: insn->code = MIR_RSHS; break;
        default: gen_assert (FALSE); break;
        }
        insn->ops[2].u.i = -insn->ops[2].u.i;
      }
      if (code == MIR_RSH || code == MIR_LSH || code == MIR_URSH) {
        if (insn->ops[2].u.i > 64) insn->ops[2].u.i = 64;
      } else if (insn->ops[2].u.i > 32) {
        insn->ops[2].u.i = 32;
      }
    }
  }
}

static uint8_t *target_translate (gen_ctx_t gen_ctx, size_t *len) {
  MIR_context_t ctx = gen_ctx->ctx;
  size_t i;
  int short_label_disp_fail_p, n_iter = 0;
  MIR_insn_t insn;
  const char *replacement;

  gen_assert (curr_func_item->item_type == MIR_func_item);
  do {
    VARR_TRUNC (uint8_t, result_code, 0);
    VARR_TRUNC (label_ref_t, label_refs, 0);
    VARR_TRUNC (uint64_t, abs_address_locs, 0);
    short_label_disp_fail_p = FALSE;
    for (insn = DLIST_HEAD (MIR_insn_t, curr_func_item->u.func->insns); insn != NULL;
         insn = DLIST_NEXT (MIR_insn_t, insn)) {
      if (insn->code == MIR_LABEL) {
        set_label_disp (gen_ctx, insn, VARR_LENGTH (uint8_t, result_code));
      } else if (insn->code != MIR_USE) {
        int use_short_label_p = TRUE;

        if (n_iter > 0 && MIR_branch_code_p (insn->code)) {
          MIR_label_t label = insn->ops[0].u.label;
          int64_t offset = (int64_t) get_label_disp (gen_ctx, label)
                           - (int64_t) VARR_LENGTH (uint8_t, result_code);

          use_short_label_p = ((offset < 0 ? -offset : offset) & ~(int64_t) 0x7fff) == 0;
        }
        replacement = find_insn_pattern_replacement (gen_ctx, insn, use_short_label_p);
        if (replacement == NULL) {
          fprintf (stderr, "fatal failure in matching insn:");
          MIR_output_insn (ctx, stderr, insn, curr_func_item->u.func, TRUE);
          exit (1);
        } else {
          gen_assert (replacement != NULL);
          out_insn (gen_ctx, insn, replacement, NULL);
        }
      }
    }
    /* Setting up labels */
    for (i = 0; i < VARR_LENGTH (label_ref_t, label_refs); i++) {
      label_ref_t lr = VARR_GET (label_ref_t, label_refs, i);

      if (lr.abs_addr_p) {
        set_int64 (&VARR_ADDR (uint8_t, result_code)[lr.label_val_disp],
                   (int64_t) get_label_disp (gen_ctx, lr.u.label));
        VARR_PUSH (uint64_t, abs_address_locs, lr.label_val_disp);
      } else if (lr.branch_type == LADDR) {
        int64_t offset
          = (int64_t) get_label_disp (gen_ctx, lr.u.label) - (int64_t) lr.label_val_disp + 4;
        int hi = offset >> 16, low = offset & 0xffff;
        if ((low & 0x8000) != 0) hi++;
        *(uint32_t *) (VARR_ADDR (uint8_t, result_code) + lr.label_val_disp) |= hi & 0xffff;
        *(uint32_t *) (VARR_ADDR (uint8_t, result_code) + lr.label_val_disp + 4) |= low;
      } else if (lr.branch_type == BRCOND) { /* 14-bit relative addressing */
        int64_t offset
          = (int64_t) get_label_disp (gen_ctx, lr.u.label) - (int64_t) lr.label_val_disp;

        gen_assert ((offset & 0x3) == 0);
        if (((offset < 0 ? -offset : offset) & ~(int64_t) 0x7fff) != 0) {
          short_label_disp_fail_p = TRUE;
        } else {
          *(uint32_t *) (VARR_ADDR (uint8_t, result_code) + lr.label_val_disp)
            |= ((offset / 4) & 0x3fff) << 2;
        }
      } else { /* 24-bit relative address */
        int64_t offset
          = (int64_t) get_label_disp (gen_ctx, lr.u.label) - (int64_t) lr.label_val_disp;
        gen_assert ((offset & 0x3) == 0
                    && ((offset < 0 ? -offset : offset) & ~(int64_t) 0x1ffffff) == 0);
        *(uint32_t *) (VARR_ADDR (uint8_t, result_code) + lr.label_val_disp)
          |= ((offset / 4) & 0xffffff) << 2;
      }
    }
    n_iter++;
  } while (short_label_disp_fail_p);
  while (VARR_LENGTH (uint8_t, result_code) % 16 != 0) /* Align the pool */
    VARR_PUSH (uint8_t, result_code, 0);
  *len = VARR_LENGTH (uint8_t, result_code);
  return VARR_ADDR (uint8_t, result_code);
}

static void target_rebase (gen_ctx_t gen_ctx, uint8_t *base) {
  MIR_code_reloc_t reloc;

  VARR_TRUNC (MIR_code_reloc_t, relocs, 0);
  for (size_t i = 0; i < VARR_LENGTH (uint64_t, abs_address_locs); i++) {
    reloc.offset = VARR_GET (uint64_t, abs_address_locs, i);
    reloc.value = base + get_int64 (base + reloc.offset);
    VARR_PUSH (MIR_code_reloc_t, relocs, reloc);
  }
  _MIR_update_code_arr (gen_ctx->ctx, base, VARR_LENGTH (MIR_code_reloc_t, relocs),
                        VARR_ADDR (MIR_code_reloc_t, relocs));
  gen_setup_lrefs (gen_ctx, base);
}

static void target_change_to_direct_calls (MIR_context_t ctx MIR_UNUSED) {}

struct target_bb_version {
  uint8_t *base;
  label_ref_t branch_ref; /* label cand used for jump to this bb version */
};

static void target_init_bb_version_data (target_bb_version_t data) {
  data->base = NULL; /* we don't know origin branch */
}

static void target_bb_translate_start (gen_ctx_t gen_ctx) {
  short_bb_branch_p = FALSE;
  VARR_TRUNC (uint8_t, result_code, 0);
  VARR_TRUNC (label_ref_t, label_refs, 0);
  VARR_TRUNC (uint64_t, abs_address_locs, 0);
}

static void target_bb_insn_translate (gen_ctx_t gen_ctx, MIR_insn_t insn, void **jump_addrs) {
  const char *replacement;
  if (insn->code == MIR_LABEL) return;
  replacement = find_insn_pattern_replacement (gen_ctx, insn, TRUE);
  gen_assert (replacement != NULL);
  out_insn (gen_ctx, insn, replacement, jump_addrs);
  if (MIR_branch_code_p (insn->code) && insn->code != MIR_JMP) short_bb_branch_p = TRUE;
}

static void target_output_jump (gen_ctx_t gen_ctx, void **jump_addrs) {
  out_insn (gen_ctx, temp_jump, temp_jump_replacement, jump_addrs);
}

static uint8_t *target_bb_translate_finish (gen_ctx_t gen_ctx, size_t *len) {
  /* add nops for possible conversion short branch or jump to branch and bctr */
  for (int i = 0; i < (short_bb_branch_p ? 13 : 6); i++) put_uint32 (gen_ctx, TARGET_NOP);
  while (VARR_LENGTH (uint8_t, result_code) % 16 != 0) /* Align the pool */
    VARR_PUSH (uint8_t, result_code, 0);
  *len = VARR_LENGTH (uint8_t, result_code);
  return VARR_ADDR (uint8_t, result_code);
}

static void setup_rel (gen_ctx_t gen_ctx, label_ref_t *lr, uint8_t *base, void *addr) {
  MIR_context_t ctx = gen_ctx->ctx;
  int64_t offset = (int64_t) addr - (int64_t) (base + lr->label_val_disp);

  gen_assert ((offset & 0x3) == 0 && !lr->abs_addr_p);
  /* ??? thread safe: */
  uint32_t *insn_ptr = (uint32_t *) (base + lr->label_val_disp), insn = *insn_ptr;
  if (lr->branch_type == BRCOND) {
    if (((offset < 0 ? -offset : offset) & ~(int64_t) 0x7fff) == 0) { /* a valid branch offset */
      insn = (insn & ~0xffff) | (((offset / 4) & 0x3fff) << 2);
      _MIR_change_code (ctx, (uint8_t *) insn_ptr, (uint8_t *) &insn, 4);
      return;
    }
    insn = (insn & ~0xffff) | (4 * 8); /* skip next jump and 6 nops for it */
    _MIR_change_code (ctx, (uint8_t *) insn_ptr, (uint8_t *) &insn, 4);
    insn = PPC_JUMP_OPCODE << (32 - 6);
    insn_ptr += 8;
    lr->branch_type = JUMP;
    lr->label_val_disp += 4 * 8;
    offset -= 4 * 8;
  }
  if (lr->branch_type == LADDR) {
    offset += 4;
    int hi = offset >> 16, low = offset & 0xffff;
    if ((low & 0x8000) != 0) hi++;
    insn |= hi & 0xffff;
    _MIR_change_code (ctx, (uint8_t *) insn_ptr, (uint8_t *) &insn, 4);
    insn_ptr++;
    insn = *insn_ptr | low;
    _MIR_change_code (ctx, (uint8_t *) insn_ptr, (uint8_t *) &insn, 4);
    return;
  } else if (lr->branch_type == JUMP) {
    if (((offset < 0 ? -offset : offset) & ~(int64_t) 0x1ffffff) == 0) { /* a valid jump offset */
      insn = (insn & ~0x3ffffff) | (((offset / 4) & 0xffffff) << 2);
      _MIR_change_code (ctx, (uint8_t *) insn_ptr, (uint8_t *) &insn, 4);
      return;
    }
    lr->branch_type = BCTR;
  }
  gen_assert (lr->branch_type == BCTR);
  VARR_TRUNC (uint8_t, result_code, 0);
  ppc64_gen_address (result_code, 12, addr); /* r12 = addr */
  insn = 0x7d8903a6;                         /* mtctr r12 */
  put_uint32 (gen_ctx, insn);
  insn = 0x4e800420; /* bctr */
  put_uint32 (gen_ctx, insn);
  _MIR_change_code (ctx, (uint8_t *) insn_ptr, VARR_ADDR (uint8_t, result_code),
                    VARR_LENGTH (uint8_t, result_code));
}

static void target_bb_rebase (gen_ctx_t gen_ctx, uint8_t *base) {
  MIR_context_t ctx = gen_ctx->ctx;
  MIR_code_reloc_t reloc;

  /* Setting up relative labels */
  for (size_t i = 0; i < VARR_LENGTH (label_ref_t, label_refs); i++) {
    label_ref_t lr = VARR_GET (label_ref_t, label_refs, i);
    if (lr.abs_addr_p) {
      _MIR_change_code (ctx, (uint8_t *) base + lr.label_val_disp, (uint8_t *) &lr.u.jump_addr, 8);
    } else {
      setup_rel (gen_ctx, &lr, base, lr.u.jump_addr);
    }
  }
  VARR_TRUNC (MIR_code_reloc_t, relocs, 0);
  for (size_t i = 0; i < VARR_LENGTH (uint64_t, abs_address_locs); i++) {
    reloc.offset = VARR_GET (uint64_t, abs_address_locs, i);
    reloc.value = base + get_int64 (base + reloc.offset);
    VARR_PUSH (MIR_code_reloc_t, relocs, reloc);
  }
  _MIR_update_code_arr (gen_ctx->ctx, base, VARR_LENGTH (MIR_code_reloc_t, relocs),
                        VARR_ADDR (MIR_code_reloc_t, relocs));
}

static void target_setup_succ_bb_version_data (gen_ctx_t gen_ctx, uint8_t *base) {
  if (VARR_LENGTH (label_ref_t, label_refs)
      != VARR_LENGTH (target_bb_version_t, target_succ_bb_versions))
    /* We can have more one possible branch from original insn
       (e.g. SWITCH, FBNE).  If it is so, we will make jumps only
       through BB thunk. */
    return;
  for (size_t i = 0; i < VARR_LENGTH (target_bb_version_t, target_succ_bb_versions); i++) {
    target_bb_version_t data = VARR_GET (target_bb_version_t, target_succ_bb_versions, i);
    if (data == NULL) continue;
    data->branch_ref = VARR_GET (label_ref_t, label_refs, i);
    data->base = base;
  }
}

static void target_redirect_bb_origin_branch (gen_ctx_t gen_ctx, target_bb_version_t data,
                                              void *addr) {
  MIR_context_t ctx = gen_ctx->ctx;

  if (data->base == NULL) return;
  if (data->branch_ref.abs_addr_p) {
    _MIR_change_code (ctx, (uint8_t *) data->base + data->branch_ref.label_val_disp,
                      (uint8_t *) &addr, 8);
  } else {
    setup_rel (gen_ctx, &data->branch_ref, data->base, addr);
  }
  data->base = NULL;
}

static void target_init (gen_ctx_t gen_ctx) {
  MIR_alloc_t alloc = gen_alloc (gen_ctx);
  MIR_context_t ctx = gen_ctx->ctx;

  gen_ctx->target_ctx = gen_malloc (gen_ctx, sizeof (struct target_ctx));
  VARR_CREATE (uint8_t, result_code, alloc, 0);
  VARR_CREATE (label_ref_t, label_refs, alloc, 0);
  VARR_CREATE (uint64_t, abs_address_locs, alloc, 0);
  VARR_CREATE (MIR_code_reloc_t, relocs, alloc, 0);
  patterns_init (gen_ctx);
  temp_jump = MIR_new_insn (ctx, MIR_JMP, MIR_new_label_op (ctx, NULL));
  temp_jump_replacement = find_insn_pattern_replacement (gen_ctx, temp_jump, FALSE);
}

static void target_finish (gen_ctx_t gen_ctx) {
  MIR_alloc_t alloc = gen_alloc (gen_ctx);
  patterns_finish (gen_ctx);
  _MIR_free_insn (gen_ctx->ctx, temp_jump);
  VARR_DESTROY (uint8_t, result_code);
  VARR_DESTROY (label_ref_t, label_refs);
  VARR_DESTROY (uint64_t, abs_address_locs);
  VARR_DESTROY (MIR_code_reloc_t, relocs);
  MIR_free (alloc, gen_ctx->target_ctx);
  gen_ctx->target_ctx = NULL;
}
