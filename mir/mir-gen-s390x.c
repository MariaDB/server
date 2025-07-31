/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

// ??? More patterns (ult, ugt, ule, uge w/o branches, multi-insn combining).

static void fancy_abort (int code) {
  if (!code) abort ();
}

#undef gen_assert
#define gen_assert(c) fancy_abort (c)

#define TARGET_EXPAND_UADDO
#define TARGET_EXPAND_UADDOS
#define TARGET_EXPAND_MULO
#define TARGET_EXPAND_MULOS
#define TARGET_EXPAND_UMULO
#define TARGET_EXPAND_UMULOS

#include <limits.h>

#include "mir-s390x.h"

static inline MIR_reg_t target_nth_loc (MIR_reg_t loc, MIR_type_t type, int n) {
  gen_assert (n == 0 || (type == MIR_T_LD && loc >= F0_HARD_REG && n == 1));
  if (n == 0) return loc;
  return loc >= F15_HARD_REG ? loc + 1 : loc + 2; /* coupled fp reg */
}

static inline int target_call_used_hard_reg_p (MIR_reg_t hard_reg, MIR_type_t type MIR_UNUSED) {
  gen_assert (hard_reg <= MAX_HARD_REG);
  return ((R0_HARD_REG <= hard_reg && hard_reg <= R5_HARD_REG) || hard_reg == R14_HARD_REG
          || (F0_HARD_REG <= hard_reg && hard_reg <= F7_HARD_REG));
}

/* Stack layout (r15(sp) refers to the last reserved stack slot
   address) from higher address to lower address memory:

        +-> Back chain
        |   area for saved f8-f15
        |   Local and spill variable area of calling function
        |   ld value area for passing args and returns
        |   Parameter area passed to called function by memory (SP + 160)
        |   Register save area for called function use:
        |      f0, f2, f4, f6 (fp argument save area)          (SP + 128)
        |      r6-r15 (other register save area)               (SP + 48)
        |      r2-r5  (argument register save area)            (SP + 16)
        |   Reserved for compiler                              (SP + 8)
SP,R11->+-- Back chain (optional)                              (SP + 0)
            Alloca area (after that new 160 bytes header should be created with new values)

SP alignment is always 8.
Originally SP(r15) and FP (r11) are the same but r15 can be changed by alloca */

#define S390X_STACK_HEADER_SIZE 160
#define S390X_GP_REG_RSAVE_AREA_START 16
#define S390X_FP_REG_ARG_SAVE_AREA_START 128

/* s390x has 3-ops insns */
static const MIR_insn_code_t target_io_dup_op_insn_codes[]
  = {MIR_ADD,   MIR_ADDS,  MIR_FADD, MIR_DADD,  MIR_SUB,  MIR_SUBS, MIR_SUBO, MIR_SUBOS,
     MIR_ADDO,  MIR_ADDOS, MIR_FSUB, MIR_DSUB,  MIR_MUL,  MIR_MULS, MIR_FMUL, MIR_DMUL,
     MIR_DIV,   MIR_DIVS,  MIR_UDIV, MIR_UDIVS, MIR_FDIV, MIR_DDIV, MIR_MOD,  MIR_MODS,
     MIR_UMOD,  MIR_UMODS, MIR_EQ,   MIR_EQS,   MIR_NE,   MIR_NES,  MIR_LSHS, MIR_RSHS,
     MIR_URSHS, MIR_AND,   MIR_ANDS, MIR_OR,    MIR_ORS,  MIR_XOR,  MIR_XORS, MIR_INSN_BOUND};

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

struct const_ref {
  size_t insn_pc;      /* where rel32 address should be in code */
  size_t next_insn_pc; /* displacement of the next insn */
  size_t const_num;
};

typedef struct const_ref const_ref_t;
DEF_VARR (const_ref_t);
struct label_ref {
  int abs_addr_p;
  size_t label_val_disp;
  union {
    MIR_label_t label;
    void *jump_addr; /* absolute addr for BBV */
  } u;
};

typedef struct label_ref label_ref_t;
DEF_VARR (label_ref_t);

struct target_ctx {
  unsigned char alloca_p, leaf_p, stack_param_p, switch_p;
  size_t param_save_area_size, blk_ld_value_save_area_size;
  MIR_insn_t temp_jump;
  const char *temp_jump_replacement;
  VARR (int) * pattern_indexes;
  VARR (insn_pattern_info_t) * insn_pattern_info;
  VARR (uint8_t) * result_code;
  VARR (uint64_t) * const_pool;
  VARR (const_ref_t) * const_refs;
  VARR (label_ref_t) * label_refs;
  VARR (uint64_t) * abs_address_locs;
  VARR (MIR_code_reloc_t) * relocs;
  VARR (uint64_t) * ld_addr_regs;
};

#define alloca_p gen_ctx->target_ctx->alloca_p
#define leaf_p gen_ctx->target_ctx->leaf_p
#define stack_param_p gen_ctx->target_ctx->stack_param_p
#define switch_p gen_ctx->target_ctx->switch_p
#define param_save_area_size gen_ctx->target_ctx->param_save_area_size
#define blk_ld_value_save_area_size gen_ctx->target_ctx->blk_ld_value_save_area_size
#define temp_jump gen_ctx->target_ctx->temp_jump
#define temp_jump_replacement gen_ctx->target_ctx->temp_jump_replacement
#define pattern_indexes gen_ctx->target_ctx->pattern_indexes
#define insn_pattern_info gen_ctx->target_ctx->insn_pattern_info
#define result_code gen_ctx->target_ctx->result_code
#define const_pool gen_ctx->target_ctx->const_pool
#define const_refs gen_ctx->target_ctx->const_refs
#define label_refs gen_ctx->target_ctx->label_refs
#define abs_address_locs gen_ctx->target_ctx->abs_address_locs
#define relocs gen_ctx->target_ctx->relocs
#define ld_addr_regs gen_ctx->target_ctx->ld_addr_regs

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
  if (save_regs > 0)
    gen_mov (gen_ctx, anchor, MIR_MOV, treg_op, _MIR_new_var_op (ctx, R2_HARD_REG));
  if (save_regs > 1)
    gen_mov (gen_ctx, anchor, MIR_MOV, treg_op2, _MIR_new_var_op (ctx, R3_HARD_REG));
  if (save_regs > 2)
    gen_mov (gen_ctx, anchor, MIR_MOV, treg_op3, _MIR_new_var_op (ctx, R4_HARD_REG));
  /* call blk move: */
  proto_item = _MIR_builtin_proto (ctx, curr_func_item->module, BLK_MOV_P, 0, NULL, 3, MIR_T_I64,
                                   "to", MIR_T_I64, "from", MIR_T_I64, "nwords");
  func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, BLK_MOV, mir_blk_mov);
  freg_op = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
  new_insn = MIR_new_insn (ctx, MIR_MOV, freg_op, MIR_new_ref_op (ctx, func_import_item));
  gen_add_insn_before (gen_ctx, anchor, new_insn);
  gen_add_insn_before (gen_ctx, anchor,
                       MIR_new_insn (gen_ctx->ctx, MIR_ADD, _MIR_new_var_op (ctx, R2_HARD_REG),
                                     _MIR_new_var_op (ctx, to_base_hard_reg),
                                     MIR_new_int_op (ctx, to_disp)));
  gen_add_insn_before (gen_ctx, anchor,
                       MIR_new_insn (gen_ctx->ctx, MIR_ADD, _MIR_new_var_op (ctx, R3_HARD_REG),
                                     _MIR_new_var_op (ctx, from_base_reg),
                                     MIR_new_int_op (ctx, from_disp)));
  gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, R4_HARD_REG),
           MIR_new_int_op (ctx, qwords));
  ops[0] = MIR_new_ref_op (ctx, proto_item);
  ops[1] = freg_op;
  ops[2] = _MIR_new_var_op (ctx, R2_HARD_REG);
  ops[3] = _MIR_new_var_op (ctx, R3_HARD_REG);
  ops[4] = _MIR_new_var_op (ctx, R4_HARD_REG);
  new_insn = MIR_new_insn_arr (ctx, MIR_CALL, 5, ops);
  gen_add_insn_before (gen_ctx, anchor, new_insn);
  /* Restore arg regs: */
  if (save_regs > 0)
    gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, R2_HARD_REG), treg_op);
  if (save_regs > 1)
    gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, R3_HARD_REG), treg_op2);
  if (save_regs > 2)
    gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, R4_HARD_REG), treg_op3);
}

static void machinize_call (gen_ctx_t gen_ctx, MIR_insn_t call_insn) {
  MIR_context_t ctx = gen_ctx->ctx;
  MIR_func_t func = curr_func_item->u.func;
  MIR_proto_t proto = call_insn->ops[0].u.ref->u.proto;
  int vararg_p = proto->vararg_p;
  size_t nargs, nops = MIR_insn_nops (ctx, call_insn), start = proto->nres + 2;
  size_t param_mem_size, call_blk_ld_value_area_size, ld_n_iregs, n_iregs, n_fregs;
  size_t qwords, blk_ld_value_disp;
  MIR_type_t type, mem_type;
  MIR_op_mode_t mode;
  MIR_var_t *arg_vars = NULL;
  MIR_op_t arg_op, temp_op, arg_reg_op, ret_op, mem_op, ret_val_op, call_res_op;
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
  n_iregs = n_fregs = param_mem_size = call_blk_ld_value_area_size = 0;
  for (size_t i = 2; i < nops; i++) {
    arg_op = call_insn->ops[i];
    /* process long double results and ld and block args to calculate memory for them: */
    if (i < start) {
      type = proto->res_types[i - 2];
    } else if (i - start < nargs) {
      type = arg_vars[i - start].type;
    } else if (arg_op.mode == MIR_OP_VAR_MEM) {
      type = arg_op.u.mem.type;
      gen_assert (MIR_all_blk_type_p (type));
    } else {
      mode = arg_op.value_mode;  // ??? smaller ints
      gen_assert (mode == MIR_OP_INT || mode == MIR_OP_UINT || mode == MIR_OP_FLOAT
                  || mode == MIR_OP_DOUBLE || mode == MIR_OP_LDOUBLE);
      if (mode == MIR_OP_FLOAT)
        (*MIR_get_error_func (ctx)) (MIR_call_op_error,
                                     "passing float variadic arg (should be passed as double)");
      type = mode == MIR_OP_DOUBLE ? MIR_T_D : mode == MIR_OP_LDOUBLE ? MIR_T_LD : MIR_T_I64;
    }
    if (type != MIR_T_LD && i < start) continue;
    if (type == MIR_T_LD)
      call_blk_ld_value_area_size += 16;
    else if (MIR_blk_type_p (type)) {
      gen_assert (arg_op.mode == MIR_OP_VAR_MEM && arg_op.u.mem.disp >= 0
                  && arg_op.u.mem.index == MIR_NON_VAR);
      call_blk_ld_value_area_size += (arg_op.u.mem.disp + 7) / 8 * 8;
    }
    if ((type == MIR_T_F || type == MIR_T_D) && n_fregs < 4) {
      /* put arguments to argument hard regs: */
      n_fregs++;
    } else if (type != MIR_T_F && type != MIR_T_D && n_iregs < 5) { /* RBLK too */
      n_iregs++;
    } else { /* put arguments on the stack */
      param_mem_size += 8;
    }
  }
  if (param_save_area_size < param_mem_size) param_save_area_size = param_mem_size;
  if (blk_ld_value_save_area_size < call_blk_ld_value_area_size)
    blk_ld_value_save_area_size = call_blk_ld_value_area_size;
  blk_ld_value_disp = param_save_area_size;
  param_mem_size = n_fregs = n_iregs = 0;
  for (size_t i = 2; i < nops; i++) { /* process args and ???long double results: */
    arg_op = call_insn->ops[i];
    gen_assert (arg_op.mode == MIR_OP_VAR
                || (arg_op.mode == MIR_OP_VAR_MEM && MIR_all_blk_type_p (arg_op.u.mem.type)));
    if (i < start) {
      type = proto->res_types[i - 2];
    } else if (i - start < nargs) {
      type = arg_vars[i - start].type;
    } else if (call_insn->ops[i].mode == MIR_OP_VAR_MEM) {
      type = call_insn->ops[i].u.mem.type;
      gen_assert (MIR_all_blk_type_p (type));
    } else {
      mode = call_insn->ops[i].value_mode;  // ??? smaller ints
      gen_assert (mode == MIR_OP_INT || mode == MIR_OP_UINT || mode == MIR_OP_DOUBLE
                  || mode == MIR_OP_LDOUBLE);
      type = mode == MIR_OP_DOUBLE ? MIR_T_D : mode == MIR_OP_LDOUBLE ? MIR_T_LD : MIR_T_I64;
    }
    if (type != MIR_T_LD && i < start) continue;
    ext_insn = NULL;
    if ((ext_code = get_ext_code (type)) != MIR_INVALID_INSN) { /* extend arg if necessary */
      temp_op = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
      ext_insn = MIR_new_insn (ctx, ext_code, temp_op, arg_op);
      call_insn->ops[i] = arg_op = temp_op;
    }
    if (type == MIR_T_LD || MIR_blk_type_p (type)) {
      if (i >= start) { /* put arg value in saved blk/ld value area: */
        if (type == MIR_T_LD) {
          mem_op = _MIR_new_var_mem_op (ctx, MIR_T_LD, blk_ld_value_disp + S390X_STACK_HEADER_SIZE,
                                        SP_HARD_REG, MIR_NON_VAR, 1);
          gen_mov (gen_ctx, call_insn, MIR_LDMOV, mem_op, arg_op);
        } else {
          qwords = (arg_op.u.mem.disp + 7) / 8;
          gen_blk_mov (gen_ctx, call_insn, S390X_STACK_HEADER_SIZE + blk_ld_value_disp, SP_HARD_REG,
                       0, arg_op.u.mem.base, qwords, n_iregs);
        }
      }
      arg_op = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
      new_insn = MIR_new_insn (ctx, MIR_ADD, arg_op, _MIR_new_var_op (ctx, SP_HARD_REG),
                               MIR_new_int_op (ctx, S390X_STACK_HEADER_SIZE + blk_ld_value_disp));
      gen_add_insn_before (gen_ctx, call_insn, new_insn);
      blk_ld_value_disp += type == MIR_T_LD ? 16 : qwords * 8;
    }
    mem_type = type == MIR_T_F || type == MIR_T_D ? type : MIR_T_I64;
    if ((type == MIR_T_F || type == MIR_T_D) && n_fregs < 4) {
      /* put arguments to argument hard regs: */
      if (ext_insn != NULL) gen_add_insn_before (gen_ctx, call_insn, ext_insn);
      arg_reg_op = _MIR_new_var_op (ctx, F0_HARD_REG + n_fregs * 2);
      gen_mov (gen_ctx, call_insn, type == MIR_T_F ? MIR_FMOV : MIR_DMOV, arg_reg_op, arg_op);
      call_insn->ops[i] = arg_reg_op;
      n_fregs++;
    } else if (type != MIR_T_F && type != MIR_T_D && n_iregs < 5) {
      if (ext_insn != NULL) gen_add_insn_before (gen_ctx, call_insn, ext_insn);
      arg_reg_op = _MIR_new_var_op (ctx, R2_HARD_REG + n_iregs);
      if (type != MIR_T_RBLK) {
        gen_mov (gen_ctx, call_insn, MIR_MOV, arg_reg_op, arg_op);
      } else {
        assert (arg_op.mode == MIR_OP_VAR_MEM);
        gen_mov (gen_ctx, call_insn, MIR_MOV, arg_reg_op, _MIR_new_var_op (ctx, arg_op.u.mem.base));
        arg_reg_op = _MIR_new_var_mem_op (ctx, MIR_T_RBLK, arg_op.u.mem.disp, R2_HARD_REG + n_iregs,
                                          MIR_NON_VAR, 1);
      }
      if (i >= start) call_insn->ops[i] = arg_reg_op; /* don't change LD return yet */
      n_iregs++;
    } else { /* put arguments on the stack: */
      if (ext_insn != NULL) gen_add_insn_before (gen_ctx, call_insn, ext_insn);
      new_insn_code = (type == MIR_T_F ? MIR_FMOV : type == MIR_T_D ? MIR_DMOV : MIR_MOV);
      mem_op = _MIR_new_var_mem_op (ctx, mem_type, param_mem_size + S390X_STACK_HEADER_SIZE,
                                    SP_HARD_REG, MIR_NON_VAR, 1);
      if (type != MIR_T_RBLK) {
        gen_mov (gen_ctx, call_insn, new_insn_code, mem_op, arg_op);
      } else {
        assert (arg_op.mode == MIR_OP_VAR_MEM);
        gen_mov (gen_ctx, call_insn, new_insn_code, mem_op,
                 _MIR_new_var_op (ctx, arg_op.u.mem.base));
      }
      if (i >= start) call_insn->ops[i] = mem_op;
      param_mem_size += 8;
    }
  }
  ld_n_iregs = n_iregs = n_fregs = 0;
  blk_ld_value_disp = param_mem_size;
  for (size_t i = 0; i < proto->nres; i++) {
    ret_op = call_insn->ops[i + 2];
    gen_assert (ret_op.mode == MIR_OP_VAR);
    type = proto->res_types[i];
    if (type == MIR_T_LD) { /* returned by address */
      new_insn_code = MIR_LDMOV;
      call_res_op = ret_val_op
        = _MIR_new_var_mem_op (ctx, MIR_T_LD, S390X_STACK_HEADER_SIZE + blk_ld_value_disp,
                               SP_HARD_REG, MIR_NON_VAR, 1);
      if (n_iregs < 5) { /* use it as a call result to keep assignment to ld_n_iregs: */
        call_res_op
          = _MIR_new_var_mem_op (ctx, MIR_T_LD, 0, R2_HARD_REG + ld_n_iregs, MIR_NON_VAR, 1);
        ld_n_iregs++;
      }
      blk_ld_value_disp += 16;
    } else if ((type == MIR_T_F || type == MIR_T_D) && n_fregs < 4) {
      new_insn_code = type == MIR_T_F ? MIR_FMOV : MIR_DMOV;
      call_res_op = ret_val_op = _MIR_new_var_op (ctx, F0_HARD_REG + n_fregs * 2);
      n_fregs++;
    } else if (type != MIR_T_F && type != MIR_T_D && n_iregs < 1) {
      new_insn_code = MIR_MOV;
      call_res_op = ret_val_op = _MIR_new_var_op (ctx, R2_HARD_REG + n_iregs);
      n_iregs++;
    } else {
      (*MIR_get_error_func (ctx)) (MIR_ret_error,
                                   "s390x can not handle this combination of return values");
    }
    new_insn = MIR_new_insn (ctx, new_insn_code, ret_op, ret_val_op);
    MIR_insert_insn_after (ctx, curr_func_item, call_insn, new_insn);
    call_insn->ops[i + 2] = call_res_op;
    if ((ext_code = get_ext_code (type)) != MIR_INVALID_INSN) {
      MIR_insert_insn_after (ctx, curr_func_item, new_insn,
                             MIR_new_insn (ctx, ext_code, ret_op, ret_op));
      new_insn = DLIST_NEXT (MIR_insn_t, new_insn);
    }
    create_new_bb_insns (gen_ctx, call_insn, DLIST_NEXT (MIR_insn_t, new_insn), call_insn);
  }
}

/* Long double insns are implemented through the following builtins: */
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
  return ((MIR_disp_t) slot * 8 + S390X_STACK_HEADER_SIZE + param_save_area_size
          + blk_ld_value_save_area_size);
}

static void set_prev_sp_reg (gen_ctx_t gen_ctx, MIR_insn_t anchor, int *prev_sp_set_p,
                             MIR_reg_t *prev_sp_reg) {
  MIR_context_t ctx = gen_ctx->ctx;
  MIR_func_t func = curr_func_item->u.func;

  if (!*prev_sp_set_p) {
    *prev_sp_set_p = TRUE;
    *prev_sp_reg = gen_new_temp_reg (gen_ctx, MIR_T_I64, func);
    gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, *prev_sp_reg),
             _MIR_new_var_mem_op (ctx, MIR_T_I64, 0, SP_HARD_REG, MIR_NON_VAR, 1));
  }
}

static MIR_reg_t target_get_stack_slot_base_reg (gen_ctx_t gen_ctx MIR_UNUSED) {
  return FP_HARD_REG;
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
  MIR_reg_t ret_reg, ld_addr_reg, prev_sp_reg;
  MIR_op_t ret_reg_op, arg_reg_op, temp_op, arg_var_op;
  int prev_sp_set_p = FALSE;
  size_t i, int_arg_num = 0, fp_arg_num = 0, disp;

  gen_assert (curr_func_item->item_type == MIR_func_item);
  func = curr_func_item->u.func;
  anchor = DLIST_HEAD (MIR_insn_t, func->insns);
  disp = S390X_STACK_HEADER_SIZE; /* param area start in the caller frame */
  VARR_TRUNC (uint64_t, ld_addr_regs, 0);
  for (i = 0; i < func->nres; i++) { /* reserve regs/space for LD result addresses */
    if (func->res_types[i] != MIR_T_LD) continue;
    ld_addr_reg = gen_new_temp_reg (gen_ctx, MIR_T_I64, func);
    VARR_PUSH (uint64_t, ld_addr_regs, ld_addr_reg);
    if (int_arg_num < 5) {
      gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, ld_addr_reg),
               _MIR_new_var_op (ctx, R2_HARD_REG + int_arg_num));
      int_arg_num++;
    } else {
      set_prev_sp_reg (gen_ctx, anchor, &prev_sp_set_p, &prev_sp_reg);
      gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, ld_addr_reg),
               _MIR_new_var_mem_op (ctx, MIR_T_I64, disp, prev_sp_reg, MIR_NON_VAR, 1));
      disp += 8;
    }
  }
  for (i = 0; i < func->nargs; i++) { /* Prologue: generate arg_var = hard_reg|stack mem ... */
    /* Argument extensions is already done in simplify */
    type = VARR_GET (MIR_var_t, func->vars, i).type;
    arg_var_op = _MIR_new_var_op (ctx, i + MAX_HARD_REG + 1);
    if ((type == MIR_T_F || type == MIR_T_D) && fp_arg_num < 4) {
      arg_reg_op = _MIR_new_var_op (ctx, F0_HARD_REG + fp_arg_num * 2);
      /* (f|d)mov arg, arg_hard_reg: */
      gen_mov (gen_ctx, anchor, type == MIR_T_F ? MIR_FMOV : MIR_DMOV, arg_var_op, arg_reg_op);
      fp_arg_num++;
    } else if (type == MIR_T_F || type == MIR_T_D) { /* (f|d)mov arg, arg_memory */
      set_prev_sp_reg (gen_ctx, anchor, &prev_sp_set_p, &prev_sp_reg);
      gen_mov (gen_ctx, anchor, type == MIR_T_F ? MIR_FMOV : MIR_DMOV, arg_var_op,
               _MIR_new_var_mem_op (ctx, type, disp + (type == MIR_T_F ? 4 : 0), prev_sp_reg,
                                    MIR_NON_VAR, 1));
      disp += 8;
    } else if (int_arg_num < 5) { /* (ld)mov arg, arg_hard_reg */
      if (type != MIR_T_LD)
        gen_mov (gen_ctx, anchor, MIR_MOV, arg_var_op,
                 _MIR_new_var_op (ctx, R2_HARD_REG + int_arg_num));
      else
        gen_mov (gen_ctx, anchor, MIR_LDMOV, arg_var_op,
                 _MIR_new_var_mem_op (ctx, type, 0, R2_HARD_REG + int_arg_num, MIR_NON_VAR, 1));
      int_arg_num++;
    } else { /* (ld)mov arg, arg_memory */
      set_prev_sp_reg (gen_ctx, anchor, &prev_sp_set_p, &prev_sp_reg);
      if (type != MIR_T_LD) {
        gen_mov (gen_ctx, anchor, MIR_MOV, arg_var_op,
                 _MIR_new_var_mem_op (ctx, MIR_T_I64, disp, prev_sp_reg, MIR_NON_VAR, 1));
      } else {
        gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, R1_HARD_REG),
                 _MIR_new_var_mem_op (ctx, MIR_T_I64, disp, prev_sp_reg, MIR_NON_VAR, 1));
        gen_mov (gen_ctx, anchor, MIR_MOV, arg_var_op,
                 _MIR_new_var_mem_op (ctx, MIR_T_LD, 0, R1_HARD_REG, MIR_NON_VAR, 1));
      }
      disp += 8;
    }
  }
  stack_param_p = disp != 0;
  switch_p = alloca_p = FALSE;
  leaf_p = TRUE;
  param_save_area_size = blk_ld_value_save_area_size = 0;
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

        gen_assert (res_reg_op.mode == MIR_OP_VAR && op_reg_op.mode == MIR_OP_VAR);
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
      MIR_op_t treg_op
        = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, curr_func_item->u.func));
      MIR_op_t va_op = insn->ops[0];
      MIR_reg_t va_reg;
      int gpr_val = 0, fpr_val = 0;
      MIR_var_t var;

      disp = 0;
      assert (func->vararg_p && (va_op.mode == MIR_OP_VAR));
      for (i = 0; i < func->nargs; i++)
        if (func->res_types[i] == MIR_T_LD) {
          if (gpr_val > 5) disp += 8;
          gpr_val++;
        }
      for (i = 0; i < func->nargs; i++) {
        var = VARR_GET (MIR_var_t, func->vars, i);
        if (var.type == MIR_T_F || var.type == MIR_T_D) {
          if (fpr_val > 4) disp += 8;
          fpr_val++;
        } else {
          if (gpr_val > 5) disp += 8;
          gpr_val++;
        }
      }
      va_reg = va_op.u.var;
      /* Insns can be not simplified as soon as they match a machine insn.  */
      /* mem64[va_reg] = gpr_val; mem64[va_reg + 8] = fpr_val */
      gen_mov (gen_ctx, insn, MIR_MOV,
               _MIR_new_var_mem_op (ctx, MIR_T_I64, 0, va_reg, MIR_NON_VAR, 1),
               MIR_new_int_op (ctx, gpr_val));
      next_insn = DLIST_PREV (MIR_insn_t, insn);
      gen_mov (gen_ctx, insn, MIR_MOV,
               _MIR_new_var_mem_op (ctx, MIR_T_I64, 8, va_reg, MIR_NON_VAR, 1),
               MIR_new_int_op (ctx, fpr_val));
      /* reg_save_area: treg = mem64[sp]; mem64[va_reg+24] = treg: */
      gen_mov (gen_ctx, insn, MIR_MOV, treg_op,
               _MIR_new_var_mem_op (ctx, MIR_T_I64, 0, SP_HARD_REG, MIR_NON_VAR, 1));
      gen_mov (gen_ctx, insn, MIR_MOV,
               _MIR_new_var_mem_op (ctx, MIR_T_I64, 24, va_reg, MIR_NON_VAR, 1), treg_op);
      /* overflow_arg_area_reg: treg = treg_op+S390X_STACK_HEADER_SIZE + disp;
         mem64[va_reg+16] = treg: */
      new_insn = MIR_new_insn (ctx, MIR_ADD, treg_op, treg_op,
                               MIR_new_int_op (ctx, S390X_STACK_HEADER_SIZE + disp));
      gen_add_insn_before (gen_ctx, insn, new_insn);
      gen_mov (gen_ctx, insn, MIR_MOV,
               _MIR_new_var_mem_op (ctx, MIR_T_I64, 16, va_reg, MIR_NON_VAR, 1), treg_op);
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
    } else if (code == MIR_RET) {
      /* In simplify we already transformed code for one return insn and added extension insns.  */
      uint32_t n_gpregs = 0, n_fregs = 0, ld_addr_n = 0;

      gen_assert (func->nres == MIR_insn_nops (ctx, insn));
      for (i = 0; i < func->nres; i++) {
        gen_assert (insn->ops[i].mode == MIR_OP_VAR);
        res_type = func->res_types[i];
        if (res_type == MIR_T_LD) { /* ldmov f1,0(addr_reg);std f1,0(r2);std f3,8(r2): */
          ld_addr_reg = VARR_GET (uint64_t, ld_addr_regs, ld_addr_n);
          gen_mov (gen_ctx, insn, MIR_LDMOV, _MIR_new_var_op (ctx, F1_HARD_REG), insn->ops[i]);
          insn->ops[i] = _MIR_new_var_mem_op (ctx, MIR_T_LD, 0, ld_addr_reg, MIR_NON_VAR, 1);
          gen_mov (gen_ctx, insn, MIR_LDMOV, insn->ops[i], _MIR_new_var_op (ctx, F1_HARD_REG));
          ld_addr_n++;
          continue;
        }
        if ((res_type == MIR_T_F || res_type == MIR_T_D) && n_fregs < 4) {
          new_insn_code = res_type == MIR_T_F ? MIR_FMOV : MIR_DMOV;
          ret_reg = F0_HARD_REG + 2 * n_fregs;
          n_fregs++;
        } else if (n_gpregs < 1) {
          ret_reg = R2_HARD_REG + n_gpregs++;
          new_insn_code = MIR_MOV;
        } else {
          (*MIR_get_error_func (ctx)) (MIR_ret_error,
                                       "s390x can not handle this combination of return values");
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
           _MIR_new_var_mem_op (gen_ctx->ctx, MIR_T_I64, disp, SP_HARD_REG, MIR_NON_VAR, 1),
           _MIR_new_var_op (gen_ctx->ctx, hard_reg));
}

static void fsave (gen_ctx_t gen_ctx, MIR_insn_t anchor, int disp, MIR_reg_t hard_reg) {
  gen_mov (gen_ctx, anchor, MIR_DMOV,
           _MIR_new_var_mem_op (gen_ctx->ctx, MIR_T_D, disp, SP_HARD_REG, MIR_NON_VAR, 1),
           _MIR_new_var_op (gen_ctx->ctx, hard_reg));
}

static void target_make_prolog_epilog (gen_ctx_t gen_ctx, bitmap_t used_hard_regs,
                                       size_t stack_slots_num) {
  MIR_context_t ctx = gen_ctx->ctx;
  MIR_func_t func;
  MIR_insn_t anchor, new_insn;
  MIR_op_t r15_reg_op, r14_reg_op, r11_reg_op, r0_reg_op;
  int saved_regs_p = FALSE;
  int64_t start_saved_fregs_offset;
  size_t i, n, frame_size, saved_fregs_num;

  gen_assert (curr_func_item->item_type == MIR_func_item);
  func = curr_func_item->u.func;
  anchor = DLIST_HEAD (MIR_insn_t, func->insns);
  if (func->vararg_p) { /* save r2-r6,f0,f2,f4,f6: */
    for (i = 0; i < 5; i++)
      isave (gen_ctx, anchor, S390X_GP_REG_RSAVE_AREA_START + i * 8, i + R2_HARD_REG);
    for (i = 0; i < 4; i++)
      fsave (gen_ctx, anchor, S390X_FP_REG_ARG_SAVE_AREA_START + i * 8, i * 2 + F0_HARD_REG);
  }
  for (i = saved_fregs_num = 0; i <= MAX_HARD_REG; i++)
    if (!target_call_used_hard_reg_p (i, MIR_T_UNDEF) && bitmap_bit_p (used_hard_regs, i)) {
      saved_regs_p = TRUE;
      if (i >= F0_HARD_REG) saved_fregs_num++;
    }
  if (leaf_p && !stack_param_p && !alloca_p && saved_regs_p == 0 && stack_slots_num == 0) return;
  r0_reg_op = _MIR_new_var_op (ctx, R0_HARD_REG);
  r11_reg_op = _MIR_new_var_op (ctx, R11_HARD_REG);
  r14_reg_op = _MIR_new_var_op (ctx, R14_HARD_REG);
  r15_reg_op = _MIR_new_var_op (ctx, R15_HARD_REG);
  /* Prologue: */
  frame_size = (param_save_area_size + S390X_STACK_HEADER_SIZE + blk_ld_value_save_area_size
                + stack_slots_num * 8);
  start_saved_fregs_offset = frame_size;
  frame_size += saved_fregs_num * 8;
  gen_assert (frame_size % 8 == 0);
  if (!func->jret_p)
    gen_mov (gen_ctx, anchor, MIR_MOV,
             _MIR_new_var_mem_op (ctx, MIR_T_I64, S390X_GP_REG_RSAVE_AREA_START + (14 - 2) * 8,
                                  R15_HARD_REG, MIR_NON_VAR, 1),
             r14_reg_op); /* mem[r15+112] = r14 */
  gen_mov (gen_ctx, anchor, MIR_MOV,
           _MIR_new_var_mem_op (ctx, MIR_T_I64, S390X_GP_REG_RSAVE_AREA_START + (11 - 2) * 8,
                                R15_HARD_REG, MIR_NON_VAR, 1),
           r11_reg_op);                        /* mem[r15+76] = r11 */
  for (i = R2_HARD_REG; i < R15_HARD_REG; i++) /* exclude r15 */
    if (!target_call_used_hard_reg_p (i, MIR_T_UNDEF) && bitmap_bit_p (used_hard_regs, i)
        && (i != 6 || !func->vararg_p))
      isave (gen_ctx, anchor, S390X_GP_REG_RSAVE_AREA_START + (i - R2_HARD_REG) * 8, i);
  gen_mov (gen_ctx, anchor, MIR_MOV, r0_reg_op, r15_reg_op); /* r0 = r15 */
  new_insn = MIR_new_insn (ctx, MIR_ADD, r15_reg_op, r15_reg_op, MIR_new_int_op (ctx, -frame_size));
  gen_add_insn_before (gen_ctx, anchor, new_insn); /* r15 -= frame_size */
  gen_mov (gen_ctx, anchor, MIR_MOV,
           _MIR_new_var_mem_op (ctx, MIR_T_I64, 0, R15_HARD_REG, MIR_NON_VAR, 1),
           r0_reg_op); /* mem[r15] = r0 */
  for (n = 0, i = F0_HARD_REG; i <= MAX_HARD_REG; i++)
    if (!target_call_used_hard_reg_p (i, MIR_T_UNDEF) && bitmap_bit_p (used_hard_regs, i))
      fsave (gen_ctx, anchor, start_saved_fregs_offset + (n++) * 8, i);
  gen_mov (gen_ctx, anchor, MIR_MOV, r11_reg_op, r15_reg_op); /* r11 = r15 */
  /* Epilogue: */
  for (anchor = DLIST_TAIL (MIR_insn_t, func->insns); anchor != NULL;
       anchor = DLIST_PREV (MIR_insn_t, anchor))
    if (anchor->code == MIR_RET || anchor->code == MIR_JRET) break;
  if (anchor == NULL) return;
  /* Restoring fp hard registers: */
  for (n = 0, i = F0_HARD_REG; i <= MAX_HARD_REG; i++)
    if (!target_call_used_hard_reg_p (i, MIR_T_UNDEF) && bitmap_bit_p (used_hard_regs, i))
      gen_mov (gen_ctx, anchor, MIR_DMOV, _MIR_new_var_op (ctx, i),
               _MIR_new_var_mem_op (ctx, MIR_T_D, start_saved_fregs_offset + (n++) * 8,
                                    R11_HARD_REG, MIR_NON_VAR, 1));
  new_insn = MIR_new_insn (ctx, MIR_ADD, r15_reg_op, r11_reg_op, MIR_new_int_op (ctx, frame_size));
  gen_add_insn_before (gen_ctx, anchor, new_insn); /* r15 = r11 + frame_size */
  /* Restore saved gp regs (including r11 and excluding r15) and r14 */
  for (i = R2_HARD_REG; i < R15_HARD_REG; i++)
    if (!target_call_used_hard_reg_p (i, MIR_T_UNDEF) && bitmap_bit_p (used_hard_regs, i))
      gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, i),
               _MIR_new_var_mem_op (ctx, MIR_T_I64,
                                    S390X_GP_REG_RSAVE_AREA_START + (i - R2_HARD_REG) * 8,
                                    SP_HARD_REG, MIR_NON_VAR, 1));
  gen_mov (gen_ctx, anchor, MIR_MOV, r11_reg_op,
           _MIR_new_var_mem_op (ctx, MIR_T_I64, S390X_GP_REG_RSAVE_AREA_START + (11 - 2) * 8,
                                R15_HARD_REG, MIR_NON_VAR, 1)); /* restore r11 */
  if (!func->jret_p)
    gen_mov (gen_ctx, anchor, MIR_MOV, r14_reg_op,
             _MIR_new_var_mem_op (ctx, MIR_T_I64, S390X_GP_REG_RSAVE_AREA_START + (14 - 2) * 8,
                                  R15_HARD_REG, MIR_NON_VAR, 1)); /* restore r14 */
}

struct pattern {
  MIR_insn_code_t code;
  /* Pattern elements:
     blank - ignore
     X - match everything
     $ - finish successfully matching
     r - register

     h<one or two decimal digits> - hard register with given number

        memory with unsigned 12-bit disp:
     m[0-2] - int (signed or unsigned) memory of size 8,16,32,64-bits
     m3 - 64-bit memory w/o index
     ms[0-2] - signed int type memory of size 8,16,32,64-bits
     mu[0-2] - unsigned int type memory of size 8,16,32,64-bits

       memory with signed 20-bit disp:
     M[0-3] - int (signed or unsigned) type memory of size 8,16,32,64-bits
     Ms[0-2] - signed int type memory of size 8,16,32,64-bits
     Mu[0-2] - unsigned int type memory of size 8,16,32,64-bits

       memory with unsigned 12-bit disp:
     mf - memory of float
     md - memory of double
     mld - memory of long double where disp + 8 is also in 12-bit range

       memory with signed 20-bit disp:
     Mf - memory of float
     Md - memory of double
     Mld - memory of long double where disp + 8 is also in 20-bit range

     i - signed 16-bit immediate
     I - any 64-bit immediate
     ua - roundup unsigned 16-bit immediate
     u[0-3] - 16-bit unsigned at pos 48,32,16,0 in 64-bit value
     un[0-3] - 16-bit unsigned at pos 48,32,16,0 in 64-bit value and all ones in others

     d - unsigned 12-bit immediate
     D - signed 20-bit immediate

     z - 0.0f immediate
     Z - 0.0 immediate

     L - reference or label which can be present by signed 32-bit pc word offset
     [0-9] - an operand matching n-th operand (n should be less than given operand number)

     Remember we have no float or (long) double immediate at this stage.  They were removed during
     simplification.  */

  const char *pattern;
  /* Bit addressing: 0..63
     Replacement elements:
     blank - ignore
     ; - insn separation

     2hex* - opcode1 [0..7] (insn of format rr)
     2hex - opcode1 [0..7] (insn of formats rx and rs)
     4hex - opcode2 [0..15] (insn of formats rre and rrfe)
     4hex* - opcode2 [0..15] (insn of sil)
     2hex:2hex - opcode1 [0..7] and opcode12 [40..47] (insn of formats rxe, rxy, and rsy)
     2hex:1hex - opcode1 [0..7] and opcode11 [12..15] (insn format ri)
     2hex:1hex* - opcode1 [0..7] and opcode11 [12..15] (insn format ril)

     s[0-2] - n-the operand reg as base reg [16..19]
     x[0-2] - n-the operand reg as index reg [12..15]
     hs<number>, hx<number> - base and index regs with given numbers
     h<number> - hardware register with given number in r1
     H<number> - hardware register with given number in r2
     r[0-2] - r1 [8..11] or R1 [24..27] for 4hex opcode
     R[0-2] - r2 [12..15] or R2 [28..31] for 4hex opcode
     n[0-2] - r1/R1 with n-th reg + 2 from MIR insn

     m = operand is (8-,16-,32-,64-bit) mem with base and index (0 reg means 0) and disp
     mn = operand is (8-,16-,32-,64-bit) mem with base and index (0 reg means 0) and disp + 8
     ma<number> - mask [8..11] (or [16..19] for 4hex opcode) with given number
     md - 12-bit unsigned [20..31]
     mD - 20-bit signed [20..39]: low part [20..31], high part [32..39]
     md<number> - md with given number
     L - label offset [16..47]
     l<number> - label with given number [16..31]

     i - 16 bit signed immediate [16..31]
     u[0-3] - 16 bit unsigned immediate starting with position 48,32,16,0 in field [16..31]
     j - 16 bit signed immediate [32..47]
     i<number> - 16 bit signed immediate with given number
     ua - roundup (i, 8)
     Ia - pc-relative address of 64-bit immediate
     sD<number> - displacement ([20..31]) used as shift
     SD<number> - displacement (low part [20..31], high part [32..39]) used as shift
     T - switch table displacement
     Q - stack header + param_area + block param area
  */
  const char *replacement;
};

/* ??? movghi */
/* Byte length: rr - 2, ri, rx, rs, rre, rrfe - 4, ril, rxe, rxy, rsy - 6 bytes */
/* The longest insn is 48-bit */
static const struct pattern patterns[] = {
  {MIR_MOV, "r r", "b904 r0 R1"}, /* lgr r0,r1 */

  {MIR_MOV, "r M3", "e3:04 r0 m"},  /* lg r0,m */
  {MIR_MOV, "r Ms2", "e3:14 r0 m"}, /* lgf r0,m */
  {MIR_MOV, "r Mu2", "e3:16 r0 m"}, /* llgf r0,m */

  {MIR_MOV, "r Ms0", "e3:77 r0 m"}, /* lgb r0,m */
  {MIR_MOV, "r Mu0", "e3:90 r0 m"}, /* llgc r0,m */

  {MIR_MOV, "r Ms1", "e3:15 r0 m"}, /* lgh r0,m */
  {MIR_MOV, "r Mu1", "e3:91 r0 m"}, /* llgh r0,m */

  {MIR_MOV, "M3 r", "e3:24 r1 m"}, /* stg r0,m */
  {MIR_MOV, "m2 r", "50 r1 m"},    /* st r0,m */
  {MIR_MOV, "M2 r", "e3:50 r1 m"}, /* sty r0,m */

  {MIR_MOV, "m1 r", "40 r1 m"},    /* sth r0,m */
  {MIR_MOV, "M1 r", "e3:70 r1 m"}, /* sthy r0,m */

  {MIR_MOV, "m0 r", "42 r1 m"},    /* stc r0,m */
  {MIR_MOV, "M0 r", "e3:72 r1 m"}, /* stcy r0,m */

  {MIR_MOV, "r i", "a7:9 r0 i"}, /* lghi r,i */

  {MIR_MOV, "m3 i", "e548* m j"}, /* mvghi m,i */

  {MIR_MOV, "r u0", "a5:f r0 u0"}, /* llill r,u */
  {MIR_MOV, "r u1", "a5:e r0 u1"}, /* llilh r,u */
  {MIR_MOV, "r u2", "a5:d r0 u2"}, /* llihl r,u */
  {MIR_MOV, "r u3", "a5:c r0 u3"}, /* llihh r,u */

  {MIR_MOV, "r D", "e3:71 r0 mD"},              /* lay r0,D */
  {MIR_MOV, "r I", "c0:0* r0 Ia; e3:04 r0 s0"}, /* larl r,pc-relative addr; lg r,0(r) */

  {MIR_FMOV, "r r", "38* r0 R1"}, /*  ler r,r */
  {MIR_DMOV, "r r", "28* r0 R1"}, /*  ldr r,r */

  {MIR_FMOV, "r z", "b374 r0"}, /*  lzer r,r */
  {MIR_DMOV, "r Z", "b375 r0"}, /*  lzdr r,r */

  {MIR_FMOV, "r mf", "78 r0 m"},    /*  le r,m */
  {MIR_DMOV, "r md", "68 r0 m"},    /*  ld r,m */
  {MIR_FMOV, "r Mf", "ed:64 r0 m"}, /*  ley r,m */
  {MIR_DMOV, "r Md", "ed:65 r0 m"}, /*  ldy r,m */

  {MIR_FMOV, "mf r", "70 r1 m"},    /* ste r,m */
  {MIR_DMOV, "md r", "60 r1 m"},    /* std r,m */
  {MIR_FMOV, "Mf r", "ed:66 r1 m"}, /* stey r,m */
  {MIR_DMOV, "Md r", "ed:67 r1 m"}, /* stdy r,m */

  {MIR_LDMOV, "r r", "b365 r0 R1"},                /* lxr r0,r1 */
  {MIR_LDMOV, "r mld", "68 r0 m; 68 n0 mn"},       /* ld r0,m;ld r0+2,disp+8-m */
  {MIR_LDMOV, "r Mld", "ed:65 r0 m; ed:65 n0 mn"}, /* ldy r0,m;ldy r0+2,disp+8-m */
  {MIR_LDMOV, "mld r", "60 r1 m; 60 n1 mn"},       /* std r1,m;std r1+2,disp+8-m */
  {MIR_LDMOV, "Mld r", "ed:67 r1 m; ed:67 n1 mn"}, /* stdy r1,m;stdy r1+2,disp+8-m */

  /* sllg r0,r1,56; srag r0,r0,56: */
  {MIR_EXT8, "r r", "eb:0d r0 R1 SD56; eb:0a r0 R0 SD56"},
  /* sllg r0,r1,56; srlg r0,r0,56: */
  {MIR_UEXT8, "r r", "eb:0d r0 R1 SD56; eb:0c r0 R0 SD56"},
  {MIR_EXT8, "r Ms0", "e3:77 r0 m"},  /* lgb r0,m */
  {MIR_UEXT8, "r Mu0", "e3:90 r0 m"}, /* llgc r0,m */

  /* sllg r0,r1,48; srag r0,r0,48: */
  {MIR_EXT16, "r r", "eb:0d r0 R1 SD48; eb:0a r0 R0 SD48"},
  /* sllg r0,r1,48; srlg r0,r0,48: */
  {MIR_UEXT16, "r r", "eb:0d r0 R1 SD48; eb:0c r0 R0 SD48"},
  {MIR_EXT16, "r Ms1", "e3:78 r0 m"},  /* lhy r0,m */
  {MIR_UEXT16, "r Mu1", "e3:91 r0 m"}, /* llgh r0,m */

  {MIR_EXT32, "r r", "b914 r0 R1"},    /* lgfr r0,r1 */
  {MIR_EXT32, "r Ms2", "e3:14 r0 m"},  /* lgf r0,m */
  {MIR_UEXT32, "r r", "b916 r0 R1"},   /* llgfr r0,r1 */
  {MIR_UEXT32, "r Mu2", "e3:16 r0 m"}, /* llgf r0,m */

  {MIR_ADDS, "r 0 r", "1a* r0 R2"},   /* ar r0,r1 */
  {MIR_ADDS, "r 0 m2", "5a r0 m"},    /* a r0,m */
  {MIR_ADD, "r 0 r", "b908 r0 R2"},   /* agr r0,r1 */
  {MIR_ADD, "r 0 M2", "e3:5a r0 m"},  /* ay r0,m */
  {MIR_ADD, "r 0 M3", "e3:08 r0 m"},  /* ag r0,m */
  {MIR_ADD, "r 0 Ms2", "e3:18 r0 m"}, /* agf r0,m */

  {MIR_ADD, "r r r", "41 r0 s1 x2"},    /* la r0,(r1,r2) */
  {MIR_ADD, "r r d", "41 r0 s1 md"},    /* la r0,d(r1) */
  {MIR_ADD, "r r D", "e3:71 r0 s1 mD"}, /* lay r0,D(r1) */

  {MIR_FADD, "r 0 r", "b30a r0 R2"},  /* aebr r0,r1*/
  {MIR_DADD, "r 0 r", "b31a r0 R2"},  /* adbr r0,r1*/
  {MIR_FADD, "r 0 mf", "ed:0a r0 m"}, /* aeb r,m*/
  {MIR_DADD, "r 0 md", "ed:1a r0 m"}, /* adb r,m*/
  // ldadd is implemented through builtin

  {MIR_SUBS, "r 0 r", "1b* r0 R2"},   /* sr r0,r1 */
  {MIR_SUBS, "r 0 m2", "5b r0 m"},    /* s r0,m */
  {MIR_SUB, "r 0 r", "b909 r0 R2"},   /* sgr r0,r1 */
  {MIR_SUB, "r 0 M2", "e3:5b r0 m"},  /* sy r0,m */
  {MIR_SUB, "r 0 M3", "e3:09 r0 m"},  /* sg r0,m */
  {MIR_SUB, "r 0 Ms2", "e3:19 r0 m"}, /* sgf r0,m */
  // ??? changing sub imm to add imm

  {MIR_ADDOS, "r 0 r", "1a* r0 R2"},   /* ar r0,r1 */
  {MIR_ADDOS, "r 0 m2", "5a r0 m"},    /* a r0,m */
  {MIR_ADDO, "r 0 r", "b908 r0 R2"},   /* agr r0,r1 */
  {MIR_ADDO, "r 0 M2", "e3:5a r0 m"},  /* ay r0,m */
  {MIR_ADDO, "r 0 M3", "e3:08 r0 m"},  /* ag r0,m */
  {MIR_ADDO, "r 0 Ms2", "e3:18 r0 m"}, /* agf r0,m */

  {MIR_SUBOS, "r 0 r", "1b* r0 R2"},   /* sr r0,r1 */
  {MIR_SUBOS, "r 0 m2", "5b r0 m"},    /* s r0,m */
  {MIR_SUBO, "r 0 r", "b909 r0 R2"},   /* sgr r0,r1 */
  {MIR_SUBO, "r 0 M2", "e3:5b r0 m"},  /* sy r0,m */
  {MIR_SUBO, "r 0 M3", "e3:09 r0 m"},  /* sg r0,m */
  {MIR_SUBO, "r 0 Ms2", "e3:19 r0 m"}, /* sgf r0,m */

  {MIR_FSUB, "r 0 r", "b30b r0 R2"},  /* sebr r0,r1*/
  {MIR_DSUB, "r 0 r", "b31b r0 R2"},  /* sdbr r0,r1*/
  {MIR_FSUB, "r 0 mf", "ed:0b r0 m"}, /* seb r,m*/
  {MIR_DSUB, "r 0 md", "ed:1b r0 m"}, /* sdb r,m*/
  // ldsub is implemented through builtin

  {MIR_MULS, "r 0 r", "b252 r0 R2"},  /* msr r0,r1 */
  {MIR_MULS, "r 0 m2", "71 r0 m"},    /* ms r0,m */
  {MIR_MULS, "r 0 M2", "e3:51 r0 m"}, /* msy r0,m */
  {MIR_MULS, "r 0 i", "a7:c r0 i"},   /* mhi r0,i */
  {MIR_MUL, "r 0 r", "b90c r0 R2"},   /* msgr r0,r1 */
  {MIR_MUL, "r 0 M2", "71 r0 m"},     /* msg r0,m */
  {MIR_MUL, "r 0 Ms2", "e3:1c r0 m"}, /* msgf r0,m */
  {MIR_MUL, "r 0 i", "a7:d r0 i"},    /* mghi r0,i */

  {MIR_FMUL, "r 0 r", "b317 r0 R2"},  /* meebr r0,r1 */
  {MIR_DMUL, "r 0 r", "b31c r0 R2"},  /* mdbr r0,r1 */
  {MIR_FMUL, "r 0 mf", "ed:17 r0 m"}, /* meeb r,m*/
  {MIR_DMUL, "r 0 md", "ed:1c r0 m"}, /* mdb r,m*/
  // ldmul is implemented through builtin

  {MIR_DIV, "h1 0 r", "b90d h0 R2"}, /* dsgr h0, r2 */
  /* lgr h1,r0; dsgr h0,r2; lgr r0,h1: */
  {MIR_DIV, "r 0 r", "b904 h1 R0; b90d h0 R2; b904 r0 H1"},
  {MIR_DIV, "h1 0 M3", "e3:0d h0 m"}, /* dsg h0, m */
  /* lgr h1,r0; dsg h0,m; lgr r0,h1: */
  {MIR_DIV, "r 0 M3", "b904 h1 R0; e3:0d h0 m; b904 r0 H1"},
  /* lgfr h1,r0; dsgfr h0,r2; lgfr r0,h1: */
  {MIR_DIVS, "r 0 r", "b914 h1 R0; b91d h0 R2; b914 r0 H1"},
  /* lgfr h1,r0; dsgf h0,m; lgfr r0,h1: */
  {MIR_DIVS, "r 0 M2", "b914 h1 R0; e3:1d h0 m; b914 r0 H1"},

  {MIR_UDIV, "h1 0 r", "a5:f h0 i0; b987 h0 R2"}, /* llill h,0; dlgr h0, r2 */
  /* llill h,0; lgr h1,r0; dlgr h0,r2; lgr r0,h1: */
  {MIR_UDIV, "r 0 r", "a5:f h0 i0; b904 h1 R0; b987 h0 R2; b904 r0 H1"},
  {MIR_UDIV, "h1 0 M3", "a5:f h0 i0; e3:87 h0 m"}, /* llill h,0; dlg h0, m */
  /* llill h,0; lgr h1,r0; dlg h0,m; lgr r0,h1: */
  {MIR_UDIV, "r 0 M3", "a5:f h0 i0; b904 h1 R0; e3:87 h0 m; b904 r0 H1"},
  /* llill h,0; llgfr h1,r0; dlr h0,r2; llgfr r0,h1: */
  {MIR_UDIVS, "r 0 r", "a5:f h0 i0; b916 h1 R0; b997 h0 R2; b916 r0 H1"},
  /* llill h,0; llgfr h1,r0; dl h0,m; llgfr r0,h1: */
  {MIR_UDIVS, "r 0 M2", "a5:f h0 i0; b916 h1 R0; e3:97 h0 m; b916 r0 H1"},

  {MIR_FDIV, "r 0 r", "b30d r0 R2"},  /* debr r0,r1 */
  {MIR_DDIV, "r 0 r", "b31d r0 R2"},  /* ddbr r0,r1 */
  {MIR_FDIV, "r 0 mf", "ed:0d r0 m"}, /* deb r,m*/
  {MIR_DDIV, "r 0 md", "ed:1d r0 m"}, /* ddb r,m*/
  // lddiv is implemented through builtin

  {MIR_MOD, "h1 0 r", "b90d h0 R2; b904 r0 H0"}, /* dsgr h0, r2; lgr r0, h0 */
  /* lgr h1,r0; dsgr h0,r2; lgr r0,h0: */
  {MIR_MOD, "r 0 r", "b904 h1 R0; b90d h0 R2; b904 r0 H0"},
  {MIR_MOD, "h1 0 M3", "e3:0d h0 m; b904 r0 H0"}, /* dsg h0, m; lgr, h0 */
  /* lgr h1,r0; dsg h0,m; lgr r0,h0: */
  {MIR_MOD, "r 0 M3", "b904 h1 R0; e3:0d h0 m; b904 r0 H0"},
  /* lgfr h1,r0; dsgfr h0,r2; lgfr r0,h0: */
  {MIR_MODS, "r 0 r", "b914 h1 R0; b91d h0 R2; b914 r0 H0"},
  /* lgfr h1,r0; dsgf h0,m; lgfr r0,h0: */
  {MIR_MODS, "r 0 M2", "b914 h1 R0; e3:1d h0 m; b914 r0 H0"},

  /* llill h,0; dlgr h0, r2; lgr r0, h0 */
  {MIR_UMOD, "h1 0 r", "a5:f h0 i0; b987 h0 R2; b904 r0 H0"},
  /* llill h,0; lgr h1,r0; dlgr h0,r2; lgr r0,h0: */
  {MIR_UMOD, "r 0 r", "a5:f h0 i0; b904 h1 R0; b987 h0 R2; b904 r0 H0"},
  /* llill h,0; dlg h0, m; lgr r0, h0: */
  {MIR_UMOD, "h1 0 M3", "a5:f h0 i0; e3:87 h0 m; b904 r0 H0"},
  /* llill h,0; lgr h1,r0; dlg h0,m; lgr r0,h0: */
  {MIR_UMOD, "r 0 M3", "a5:f h0 i0; b904 h1 R0; e3:87 h0 m; b904 r0 H0"},
  /* llill h,0; llgfr h1,r0; dlr h0,r2; llgfr r0,h0: */
  {MIR_UMODS, "r 0 r", "a5:f h0 i0; b916 h1 R0; b997 h0 R2; b916 r0 H0"},
  /* llill h,0; llgfr h1,r0; dl h0,m; llgfr r0,h0: */
  {MIR_UMODS, "r 0 M2", "a5:f h0 i0; b916 h1 R0; e3:97 h0 m; b916 r0 H0"},
// all ld insn are changed to builtins

/* lghi r0,1; jmask<m> L; lghi r0,0 */
#define CMPEND(m) "; a7:9 r0 i1; a7:4 ma" #m " l8; a7:9 r0 i0"

  /* (xgr r0,r2 | xg r0,m); lpgr r0,r0; aghi r0,-1; srlg r0,r0,63: */
  {MIR_EQ, "r 0 r", "b982 r0 R2; b900 r0 R0; a7:b r0 i65535; eb:0c r0 R0 SD63"},
  {MIR_EQ, "r 0 M3", "e3:82 r0 m; b900 r0 R0; a7:b r0 i65535; eb:0c r0 R0 SD63"},
  /* (xr r0,r2 | x r0,m | xy r0, m); lpr r0,r0; ahi r0,-1; srl r0,r0,31: */
  {MIR_EQS, "r 0 r", "17* r0 R2; 10* r0 R0; a7:a r0 i65535; 88 r0 R0 Sd31"},
  {MIR_EQS, "r 0 m2", "57 r0 m; 10* r0 R0; a7:a r0 i65535; 88 r0 R0 Sd31"},
  {MIR_EQS, "r 0 M2", "e3:57 r0 m; 10* r0 R0; a7:a r0 i65535; 88 r0 R0 Sd31"},
  /* (cer r1,r2 | ce r1, mf); lghi r0,1; je L; lghi r0,0: */
  {MIR_FEQ, "r r r", "b309 r1 R2" CMPEND (8)},
  {MIR_FEQ, "r r mf", "ed:09 r1 m" CMPEND (8)},
  /* (cdbr r1,r2 | cdb r1, mf); lghi r0,1; je L; lghi r0,0: */
  {MIR_DEQ, "r r r", "b319 r1 R2" CMPEND (8)},
  {MIR_DEQ, "r r md", "ed:19 r1 m" CMPEND (8)},

  /* (xgr r0,r2 | xg r0,m); lngr r0,r0; srlg r0,r0,63: */
  {MIR_NE, "r 0 r", "b982 r0 R2; b901 r0 R0; eb:0c r0 R0 SD63"},
  {MIR_NE, "r 0 M3", "e3:82 r0 m; b901 r0 R0; eb:0c r0 R0 SD63"},
  /* (xr r0,r2 | x r0,m | xy r0, m); lnr r0,r0; srl r0,r0,31: */
  {MIR_NES, "r 0 r", "17* r0 R2; 11* r0 R0; 88 r0 R0 Sd31"},
  {MIR_NES, "r 0 m2", "57 r0 m; 11* r0 R0; 88 r0 R0 Sd31"},
  {MIR_NES, "r 0 M2", "e3:57 r0 m; 11* r0 R0; 88 r0 R0 Sd31"},

  /* (cer r1,r2 | ce r1, mf); lghi r0,1; j<lt, gt, un> L; lghi r0,0: */
  {MIR_FNE, "r r r", "b309 r1 R2" CMPEND (7)},
  {MIR_FNE, "r r mf", "ed:09 r1 m" CMPEND (7)},
  /* (cdbr r1,r2 | cdb r1, mf); lghi r0,1; j<lt, gt, un> L; lghi r0,0: */
  {MIR_DNE, "r r r", "b319 r1 R2" CMPEND (7)},
  {MIR_DNE, "r r md", "ed:19 r1 m" CMPEND (7)},

#define CMP(LC, SC, ULC, USC, FC, DC, m)                                                 \
  {LC, "r r r", "b920 r1 R2" CMPEND (m)},      /* cgr r1,r2;lghi r0,1;jm L;lghi r0,0 */  \
    {LC, "r r M3", "e3:20 r1 m" CMPEND (m)},   /* cg r1,m;lghi r0,1;jm L;lghi r0,0 */    \
    {LC, "r r Ms2", "e3:30 r1 m" CMPEND (m)},  /* cgf r1,m;lghi r0,1;jm L;lghi r0,0 */   \
    {SC, "r r r", "19* r1 R2" CMPEND (m)},     /* cr r1,r2;lghi r0,1;jm L;lghi r0,0 */   \
    {SC, "r r m2", "59 r1 m" CMPEND (m)},      /* c r1,m;lghi r0,1;jm L;lghi r0,0 */     \
    {SC, "r r M2", "e3:59 r1 m" CMPEND (m)},   /* cy r1,m;lghi r0,1;jm L;lghi r0,0 */    \
    {ULC, "r r r", "b921 r1 R2" CMPEND (m)},   /* clgr r1,r2;lghi r0,1;jm L;lghi r0,0 */ \
    {ULC, "r r M3", "e3:21 r1 m" CMPEND (m)},  /* clg r1,m;lghi r0,1;jm L;lghi r0,0 */   \
    {ULC, "r r Mu2", "e3:31 r1 m" CMPEND (m)}, /* clgf r1,m;lghi r0,1;jm L;lghi r0,0 */  \
    {USC, "r r r", "15* r1 R2" CMPEND (m)},    /* clr r1,r2;lghi r0,1;jm L;lghi r0,0 */  \
    {USC, "r r m2", "55 r1 m" CMPEND (m)},     /* cl r1,m;lghi r0,1;jm L;lghi r0,0 */    \
    {USC, "r r M2", "e3:55 r1 m" CMPEND (m)},  /* cly r1,m;lghi r0,1;jm L;lghi r0,0 */   \
    {FC, "r r r", "b309 r1 R2" CMPEND (m)},    /* cer r1,r2;lghi r0,1;jm L;lghi r0,0 */  \
    {FC, "r r mf", "ed:09 r1 m" CMPEND (m)},   /* ce r1,mf;lghi r0,1;jm L;lghi r0,0 */   \
    {DC, "r r r", "b319 r1 R2" CMPEND (m)},    /* cdbr r1,r2;lghi r0,1;jm L;lghi r0,0 */ \
    {DC, "r r md", "ed:19 r1 m" CMPEND (m)},   /* cdb r1,mf;lghi r0,1;jm L;lghi r0,0 */

  CMP (MIR_LT, MIR_LTS, MIR_ULT, MIR_ULTS, MIR_FLT, MIR_DLT, 4)
    CMP (MIR_GT, MIR_GTS, MIR_UGT, MIR_UGTS, MIR_FGT, MIR_DGT, 2)
      CMP (MIR_LE, MIR_LES, MIR_ULE, MIR_ULES, MIR_FLE, MIR_DLE, 12)
        CMP (MIR_GE, MIR_GES, MIR_UGE, MIR_UGES, MIR_FGE, MIR_DGE, 10)

#define SBRCL(mask) "c0:4* ma" #mask " L"
#define BRCL(mask) "; " SBRCL (mask)

          {MIR_JMP, "L", SBRCL (15)}, /* bcril m15, l */

  {MIR_LADDR, "r L", "c0:0* r0 L"}, /* lalr r,offset */
  {MIR_JMPI, "r", "07* ma15 R0"},   /* br r */

  {MIR_BT, "L r", "b902 r1 R1" BRCL (6)}, /* ltgr r0,r0; bcril m8,l */
  {MIR_BF, "L r", "b902 r1 R1" BRCL (8)}, /* ltgr r1,r1; bcril m6,l */
  {MIR_BTS, "L r", "12* r1 R1" BRCL (6)}, /* ltr r0,r0; bcril m8,l */
  {MIR_BFS, "L r", "12* r1 R1" BRCL (8)}, /* ltr r1,r1; bcril m6,l */

#define BCMP(LC, SC, FC, DC, m, fm)                                    \
  {LC, "L r r", "b920 r1 R2" BRCL (m)},     /* cgr r1,r2; bcril m,l */ \
    {LC, "L r M3", "e3:20 r1 m" BRCL (m)},  /* cg r1,m; bcril m,l */   \
    {LC, "L r Ms2", "e3:30 r1 m" BRCL (m)}, /* cgf r1,m; bcril m,l */  \
    {SC, "L r r", "19* r1 R2" BRCL (m)},    /* cr r1,r2; bcril m,l */  \
    {SC, "L r m2", "59 r1 m" BRCL (m)},     /* c r1,m; bcril m,l */    \
    {SC, "L r M2", "e3:59 r1 m" BRCL (m)},  /* cy r1,m; bcril m,l */   \
    {FC, "L r r", "b309 r1 R2" BRCL (fm)},  /* cer r1,r2; bcril L */   \
    {FC, "L r mf", "ed:09 r1 m" BRCL (fm)}, /* ce r1, mf; bcril L */   \
    {DC, "L r r", "b319 r1 R2" BRCL (fm)},  /* cdbr r1,r2; bcril L */  \
    {DC, "L r md", "ed:19 r1 m" BRCL (fm)}, /* cdb r1, md; bcril L: */

  BCMP (MIR_BEQ, MIR_BEQS, MIR_FBEQ, MIR_DBEQ, 8, 8)
    BCMP (MIR_BNE, MIR_BNES, MIR_FBNE, MIR_DBNE, 6, 7) /* only fp ne has unordered mask bit */
  BCMP (MIR_BLT, MIR_BLTS, MIR_FBLT, MIR_DBLT, 4, 4)
    BCMP (MIR_BGT, MIR_BGTS, MIR_FBGT, MIR_DBGT, 2, 2)
      BCMP (MIR_BLE, MIR_BLES, MIR_FBLE, MIR_DBLE, 12, 12)
        BCMP (MIR_BGE, MIR_BGES, MIR_FBGE, MIR_DBGE, 10, 10)

#define BCMPU(LC, SC, m)                                                \
  {LC, "L r r", "b921 r1 R2" BRCL (m)},     /* clgr r1,r2; bcril m,l */ \
    {LC, "L r M3", "e3:21 r1 m" BRCL (m)},  /* clg r1,m; bcril m,l */   \
    {LC, "L r Ms2", "e3:31 r1 m" BRCL (m)}, /* clgf r1,m; bcril m,l */  \
    {SC, "L r r", "15* r1 R2" BRCL (m)},    /* clr r1,r2; bcril m,l */  \
    {SC, "L r m2", "55 r1 m" BRCL (m)},     /* cl r1,m; bcril m,l */    \
    {SC, "L r M2", "e3:55 r1 m" BRCL (m)},  /* cly r1,m; bcril m,l */

          BCMPU (MIR_UBLT, MIR_UBLTS, 4) BCMPU (MIR_UBGT, MIR_UBGTS, 2)
            BCMPU (MIR_UBLE, MIR_UBLES, 12) BCMPU (MIR_UBGE, MIR_UBGES, 10)

              {MIR_BO, "L", SBRCL (1)}, /* jo l */
  {MIR_BNO, "L", SBRCL (14)},           /* jno l */

  {MIR_NEG, "r r", "b903 r0 R1"},  /* lcgr r0,r1 */
  {MIR_NEGS, "r r", "13* r0 R1"},  /* lcr r0,r1 */
  {MIR_FNEG, "r r", "b303 r0 R1"}, /* lcebr r0,r1 */
  {MIR_DNEG, "r r", "b313 r0 R1"}, /* lcdbr r0,r1 */
                                   // ldneg is a builtin

  {MIR_LSH, "r r r", "eb:0d r0 R1 s2"}, /* sllg r0,r2,b3 */
  {MIR_LSH, "r r D", "eb:0d r0 R1 mD"}, /* sllg r0,r2,d */
  {MIR_LSHS, "r 0 r", "89 r0 s2"},      /* sll r0,b2 */
  {MIR_LSHS, "r 0 d", "89 r0 md"},      /* sll r0,r2,d */

  {MIR_RSH, "r r r", "eb:0a r0 R1 s2"}, /* srag r0,r2,b3 */
  {MIR_RSH, "r r D", "eb:0a r0 R1 mD"}, /* srag r0,r2,d */
  {MIR_RSHS, "r 0 r", "8a r0 s2"},      /* sra r0,b2 */
  {MIR_RSHS, "r 0 d", "8a r0 md"},      /* sra r0,r2,d */

  {MIR_URSH, "r r r", "eb:0c r0 R1 s2"}, /* srlg r0,r2,b3 */
  {MIR_URSH, "r r D", "eb:0c r0 R1 mD"}, /* srlg r0,r2,d */
  {MIR_URSHS, "r 0 r", "88 r0 s2"},      /* srl r0,b2 */
  {MIR_URSHS, "r 0 d", "88 r0 md"},      /* srl r0,r2,d */

  {MIR_AND, "r 0 r", "b980 r0 R2"},    /* ngr r0,r1 */
  {MIR_AND, "r 0 M3", "e3:80 r0 m"},   /* ng r0,m */
  {MIR_AND, "r 0 un0", "a5:7 r0 u0"},  /* nill r0,u */
  {MIR_AND, "r 0 un1", "a5:6 r0 u1"},  /* nilh r0,u */
  {MIR_AND, "r 0 un2", "a5:5 r0 u2"},  /* nihl r0,u */
  {MIR_AND, "r 0 un3", "a5:4 r0 u3"},  /* nihh r0,u */
  {MIR_ANDS, "r 0 r", "14* r0 R2"},    /* nr r0,r1 */
  {MIR_ANDS, "r 0 m2", "54 r0 m"},     /* n r0,m */
  {MIR_ANDS, "r 0 M2", "e3:54 r0 m"},  /* ny r0,m */
  {MIR_ANDS, "r 0 un0", "a5:7 r0 u0"}, /* nill r0,u */
  {MIR_ANDS, "r 0 un1", "a5:6 r0 u1"}, /* nilh r0,u */

  {MIR_OR, "r 0 r", "b981 r0 R2"},   /* ogr r0,r1 */
  {MIR_OR, "r 0 M3", "e3:81 r0 m"},  /* og r0,m */
  {MIR_OR, "r 0 u0", "a5:b r0 u0"},  /* oill r0,u */
  {MIR_OR, "r 0 u1", "a5:a r0 u1"},  /* oilh r0,u */
  {MIR_OR, "r 0 u2", "a5:9 r0 u2"},  /* oihl r0,u */
  {MIR_OR, "r 0 u3", "a5:8 r0 u3"},  /* oihh r0,u */
  {MIR_ORS, "r 0 r", "16* r0 R2"},   /* or r0,r1 */
  {MIR_ORS, "r 0 m2", "56 r0 m"},    /* o r0,m */
  {MIR_ORS, "r 0 M2", "e3:56 r0 m"}, /* oy r0,m */
  {MIR_ORS, "r 0 u0", "a5:b r0 u0"}, /* oill r0,u */
  {MIR_ORS, "r 0 u1", "a5:a r0 u1"}, /* oilh r0,u */

  {MIR_XOR, "r 0 r", "b982 r0 R2"},   /* xgr r0,r1 */
  {MIR_XOR, "r 0 M3", "e3:82 r0 m"},  /* xg r0,m */
  {MIR_XORS, "r 0 r", "17* r0 R2"},   /* xr r0,r1 */
  {MIR_XORS, "r 0 m2", "57 r0 m"},    /* x r0,m */
  {MIR_XORS, "r 0 M2", "e3:57 r0 m"}, /* xy r0,m */

  {MIR_I2F, "r r", "b3a4 r0 R1"},  /* cegbr r0,r1 */
  {MIR_I2D, "r r", "b3a5 r0 R1"},  /* cdgbr r0,r1 */
  {MIR_UI2F, "r r", "b3a0 r0 R1"}, /* celgbr r0,r1 */
  {MIR_UI2D, "r r", "b3a1 r0 R1"}, /* cdlgbr r0,r1 */

  {MIR_F2I, "r r", "b3a8 ma5 r0 R1"}, /* cgebr r0,5,r1 */
  {MIR_D2I, "r r", "b3a9 ma5 r0 R1"}, /* cgdbr r0,5,r1 */
  {MIR_F2D, "r r", "b304 r0 R1"},     /* ldebr r0,r1 */
  {MIR_D2F, "r r", "b344 r0 R1"},     /* ledbr r0,r1 */
  // i2ld, ui2ld, ld2i, f2ld, d2ld, ld2f, ld2d are builtins

  {MIR_CALL, "X r $", "0d* h14 R1"}, /* basr h14,r0 */
  {MIR_RET, "$", "07* ma15 H14"},    /* bcr m15,14 */

  {MIR_JCALL, "X r $", "07* ma15 R1"}, /* br r */
  {MIR_JRET, "r $", "07* ma15 R0"},    /* br r */

/* sgr h15,r0; lg h0,(h15,r0); stg h0,0(h15); lay r0,160+param_area_size+blkparamsize(h15): */
#define ALLOCA_END "; b909 h15 R0; e3:04 h0 hs15 x0; e3:24 h0 hs15; e3:71 r0 Q hs15"

  /* la r0,7(r1);nill r0,0xfff8; ... : */
  {MIR_ALLOCA, "r r", "e3:71 r0 s1 md7; a5:7 r0 i65528" ALLOCA_END},
  /* lllill r0,ua; ...: */
  {MIR_ALLOCA, "r ua", "a5:f r0 ua" ALLOCA_END},

  {MIR_BSTART, "r", "b904 r0 H15"}, /* lgr r0,h15 */
  /* lg h0,0(h15);lgr h15,r0; stg h0,0(r15): */
  {MIR_BEND, "r", "e3:04 h0 hs15; b904 h15 R0; e3:24 h0 hs15"},

  /* sllg h4,r0,3; lalr h5,T; lg h4,0(h4,h5); br h4; TableContent: */
  {MIR_SWITCH, "r $", "eb:0d h4 R0 SD3; c0:0* h5 T; e3:04 h4 hs4 hx5; 07* ma15 H4"},
};

static void target_get_early_clobbered_hard_regs (MIR_insn_t insn, MIR_reg_t *hr1, MIR_reg_t *hr2) {
  MIR_insn_code_t code = insn->code;

  *hr1 = *hr2 = MIR_NON_VAR;
  if (code == MIR_DIV || code == MIR_DIVS || code == MIR_UDIV || code == MIR_UDIVS
      || code == MIR_MOD || code == MIR_MODS || code == MIR_UMOD || code == MIR_UMODS) {
    *hr1 = R0_HARD_REG;
    *hr2 = R1_HARD_REG;
  } else if (code == MIR_ULE || code == MIR_ULES || code == MIR_UGE || code == MIR_UGES
             || code == MIR_ALLOCA) {
    *hr1 = R0_HARD_REG;
  } else if (code == MIR_CALL) {  // ??? to strict: additional output, not early clobber
    *hr1 = R14_HARD_REG;
  } else if (code == MIR_SWITCH) {
    *hr1 = R4_HARD_REG;
    *hr2 = R5_HARD_REG;
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
  gen_assert (prev_code != MIR_INSN_BOUND);
  info_addr[prev_code].num = n - info_addr[prev_code].start;
}

static int int20_p (int64_t i) { return -(1 << 19) <= i && i < (1 << 19); }
static int uint12_p (uint64_t u) { return !(u >> 12); }
static int int16_p (int64_t i) { return -(1 << 15) <= i && i < (1 << 15); }
static int uint16_p (uint64_t u) { return !(u >> 16); }
static int nth_uint16_p (uint64_t u, int n) { return !(u & ~(((uint64_t) 0xffff) << n * 16)); }

static int pattern_match_p (gen_ctx_t gen_ctx, const struct pattern *pat, MIR_insn_t insn) {
  MIR_context_t ctx = gen_ctx->ctx;
  int n;
  size_t nop, nops = MIR_insn_nops (ctx, insn);
  const char *p;
  char ch, start_ch;
  MIR_op_mode_t mode;
  MIR_op_t op, original;
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
      if (op.mode != MIR_OP_VAR) return FALSE;
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
      int l_p = FALSE, u_p = TRUE, s_p = TRUE, index_p = TRUE;

      if (op.mode != MIR_OP_VAR_MEM) return FALSE;
      ch = *++p;
      switch (ch) {
      case 'f':
        type = MIR_T_F;
        type2 = MIR_T_BOUND;
        break;
      case 'd':
        type = MIR_T_D;
        type2 = MIR_T_BOUND;
        break;
      case 'l':
        ch = *++p;
        gen_assert (ch == 'd');
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
          index_p = start_ch != 'm'; /* m3 special treatment */
          type = u_p ? MIR_T_U64 : MIR_T_I64;
          type2 = u_p && s_p ? MIR_T_I64 : MIR_T_BOUND;
#if MIR_PTR64
          type3 = MIR_T_P;
#endif
        }
      }
      if (op.u.var_mem.type != type && op.u.var_mem.type != type2 && op.u.var_mem.type != type3)
        return FALSE;
      if ((!index_p && op.u.var_mem.base != MIR_NON_VAR && op.u.var_mem.index != MIR_NON_VAR)
          || (op.u.var_mem.index != MIR_NON_VAR && op.u.var_mem.scale != 1)
          || op.u.var_mem.base == R0_HARD_REG || op.u.var_mem.index == R0_HARD_REG
          || !((start_ch == 'm' && uint12_p (op.u.var_mem.disp))
               || (start_ch != 'm' && int20_p (op.u.var_mem.disp)))
          || (l_p
              && !((start_ch == 'm' && uint12_p (op.u.var_mem.disp + 8))
                   || (start_ch != 'm' && int20_p (op.u.var_mem.disp + 8)))))
        return FALSE;
      break;
    }
    case 'i':
      if ((op.mode != MIR_OP_INT && op.mode != MIR_OP_UINT) || !int16_p (op.u.i)) return FALSE;
      break;
    case 'I':
      if (op.mode != MIR_OP_INT && op.mode != MIR_OP_UINT && op.mode != MIR_OP_REF) return FALSE;
      break;
    case 'u':
      if (op.mode != MIR_OP_INT && op.mode != MIR_OP_UINT) return FALSE;
      ch = *++p;
      if (ch == 'a') {
        if (!uint16_p ((op.u.u + 7) / 8 * 8)) return FALSE;
      } else if ('0' <= ch && ch <= '3') {
        if (!nth_uint16_p (op.u.u, ch - '0')) return FALSE;
      } else if (ch == 'n') {
        ch = *++p;
        gen_assert ('0' <= ch && ch <= '3');
        if (!nth_uint16_p (~op.u.u, ch - '0')) return FALSE;
      } else {
        p--;
        if (!uint16_p (op.u.u)) return FALSE;
      }
      break;
    case 'd':
      if ((op.mode != MIR_OP_INT && op.mode != MIR_OP_UINT) || !uint12_p (op.u.u)) return FALSE;
      break;
    case 'D':
      if ((op.mode != MIR_OP_INT && op.mode != MIR_OP_UINT) || !int20_p (op.u.i)) return FALSE;
      break;
    case 'z':
      if (op.mode != MIR_OP_FLOAT || op.u.f == 0.0f) return FALSE;
      break;
    case 'Z':
      if (op.mode != MIR_OP_DOUBLE || op.u.d == 0.0) return FALSE;
      break;
    case 'L':
      if (op.mode != MIR_OP_LABEL && op.mode != MIR_OP_REF) return FALSE;
      break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      n = start_ch - '0';
      gen_assert (n < (int) nop);
      original = insn->ops[n];
      mode = op.mode;
      if (mode == MIR_OP_UINT) mode = MIR_OP_INT;
      if (original.mode != mode && (original.mode != MIR_OP_UINT || mode != MIR_OP_INT))
        return FALSE;
      gen_assert (mode == MIR_OP_VAR || mode == MIR_OP_INT || mode == MIR_OP_FLOAT
                  || mode == MIR_OP_DOUBLE || mode == MIR_OP_LDOUBLE || mode == MIR_OP_VAR_MEM
                  || mode == MIR_OP_LABEL);
      if (mode == MIR_OP_VAR && op.u.var != original.u.var)
        return FALSE;
      else if (mode == MIR_OP_INT && op.u.i != original.u.i)
        return FALSE;
      else if (mode == MIR_OP_FLOAT && op.u.f != original.u.f)
        return FALSE;
      else if (mode == MIR_OP_DOUBLE && op.u.d != original.u.d)
        return FALSE;
      else if (mode == MIR_OP_LDOUBLE && op.u.ld != original.u.ld)
        return FALSE;
      else if (mode == MIR_OP_LABEL && op.u.label != original.u.label)
        return FALSE;
      else if (mode == MIR_OP_VAR_MEM
               && (op.u.var_mem.type != original.u.var_mem.type
                   || op.u.var_mem.scale != original.u.var_mem.scale
                   || op.u.var_mem.base != original.u.var_mem.base
                   || op.u.var_mem.index != original.u.var_mem.index
                   || op.u.var_mem.disp != original.u.var_mem.disp))
        return FALSE;
      break;
    default: gen_assert (FALSE);
    }
  }
  gen_assert (nop == nops);
  return TRUE;
}

static const char *find_insn_pattern_replacement (gen_ctx_t gen_ctx, MIR_insn_t insn) {
  int i;
  const struct pattern *pat;
  insn_pattern_info_t info = VARR_GET (insn_pattern_info_t, insn_pattern_info, insn->code);

  for (i = 0; i < info.num; i++) {
    pat = &patterns[VARR_GET (int, pattern_indexes, info.start + i)];
    if (pattern_match_p (gen_ctx, pat, insn)) return pat->replacement;
  }
  return NULL;
}

static void patterns_finish (gen_ctx_t gen_ctx) {
  VARR_DESTROY (int, pattern_indexes);
  VARR_DESTROY (insn_pattern_info_t, insn_pattern_info);
}

static int dec_value (int ch) { return '0' <= ch && ch <= '9' ? ch - '0' : -1; }

static int hex_value (int ch) {
  if ('0' <= ch && ch <= '9') return ch - '0';
  if ('a' <= ch && ch <= 'f') return ch - 'a' + 10;
  if ('A' <= ch && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

static uint64_t read_dec (const char **ptr) {
  int v;
  const char *p = *ptr + 1;
  uint64_t res = 0;

  if (dec_value (*p) < 0) return -1;
  for (p = *ptr + 1; (v = dec_value (*p)) >= 0; p++) {
    gen_assert ((res >> 60) == 0);
    res = res * 10 + v;
  }
  gen_assert (p != *ptr);
  *ptr = p - 1;
  return res;
}

static int read_curr_hex (const char **ptr, int *v) {
  const char *p;
  int n = 0, d;

  for (*v = 0, p = *ptr; (d = hex_value (*p)) >= 0; p++, n++) {
    gen_assert (n < 4);
    *v = *v * 16 + d;
  }
  if (n != 0) *ptr = p - 1;
  return n; /* number of consumed hex digits */
}

static void put_arr (struct gen_ctx *gen_ctx, void *v, size_t len) { /* BE only */
  for (size_t i = 0; i < len; i++) VARR_PUSH (uint8_t, result_code, ((uint8_t *) v)[i]);
}

static void set_int32 (uint8_t *addr, int32_t v) { *(int32_t *) addr = v; }

static void set_int64 (uint8_t *addr, int64_t v) { *(int64_t *) addr = v; }

static int64_t get_int64 (uint8_t *addr) { return *(int64_t *) addr; }

static size_t add_to_const_pool (struct gen_ctx *gen_ctx, uint64_t v) {
  uint64_t *addr = VARR_ADDR (uint64_t, const_pool);
  size_t n, len = VARR_LENGTH (uint64_t, const_pool);

  for (n = 0; n < len; n++)
    if (addr[n] == v) return n;
  VARR_PUSH (uint64_t, const_pool, v);
  return len;
}

static int setup_imm_addr (struct gen_ctx *gen_ctx, uint64_t v) {
  const_ref_t cr;
  size_t n;

  n = add_to_const_pool (gen_ctx, v);
  cr.insn_pc = 0;
  cr.next_insn_pc = 0;
  cr.const_num = n;
  VARR_PUSH (const_ref_t, const_refs, cr);
  return VARR_LENGTH (const_ref_t, const_refs) - 1;
}

static uint64_t get_op_imm (gen_ctx_t gen_ctx, MIR_op_t op) {
  if (op.mode == MIR_OP_INT || op.mode == MIR_OP_UINT) return op.u.u;
  gen_assert (op.mode == MIR_OP_REF);
  if (op.u.ref->item_type == MIR_data_item && op.u.ref->u.data->name != NULL
      && _MIR_reserved_ref_name_p (gen_ctx->ctx, op.u.ref->u.data->name))
    return (uint64_t) op.u.ref->u.data->u.els;
  return (uint64_t) op.u.ref->addr;
}

static uint64_t get_imm (gen_ctx_t gen_ctx, MIR_insn_t insn) {
  MIR_op_t *ops = insn->ops;
  if (insn->nops >= 2
      && (ops[1].mode == MIR_OP_INT || ops[1].mode == MIR_OP_UINT || ops[1].mode == MIR_OP_REF))
    return get_op_imm (gen_ctx, ops[1]);
  if (insn->nops >= 3
      && (ops[2].mode == MIR_OP_INT || ops[2].mode == MIR_OP_UINT || ops[2].mode == MIR_OP_REF))
    return get_op_imm (gen_ctx, ops[2]);
  gen_assert (FALSE);
  return 0;
}

static uint64_t place_field (uint64_t v, int start_bit, int len) {
  gen_assert (start_bit >= 0 && len > 0 && start_bit + len <= 64);
  return (v & (-(uint64_t) 1 >> (64 - len))) << (64 - start_bit - len);
}

static void set_insn_field (uint64_t *binsn, uint64_t v, int start_bit, int len) {
  *binsn |= place_field (v, start_bit, len);
}

static void check_and_set_mask (uint64_t *binsn_mask, uint64_t mask, int start_bit, int len) {
  gen_assert ((*binsn_mask & place_field (mask, start_bit, len)) == 0);
  *binsn_mask |= place_field (mask, start_bit, len);
}

static void out_insn (gen_ctx_t gen_ctx, MIR_insn_t insn, const char *replacement,
                      void **jump_addrs) {
  MIR_context_t ctx = gen_ctx->ctx;
  size_t nops = MIR_insn_nops (ctx, insn);
  size_t offset;
  const char *p, *insn_str;
  label_ref_t lr;
  int switch_table_addr_insn_start = -1;
  uint64_t zero64 = 0;
  uint16_t nop_binsn = 0x18 << 8; /* lr 0,0 */

  if (insn->code == MIR_ALLOCA
      && (insn->ops[1].mode == MIR_OP_INT || insn->ops[1].mode == MIR_OP_UINT))
    insn->ops[1].u.u = (insn->ops[1].u.u + 15) & -16;
  for (insn_str = replacement;; insn_str = p + 1) {
    MIR_op_t op;
    char ch, ch2, start_ch;
    uint64_t u, binsn = 0, binsn_mask = 0;
    int opcode1 = -1, opcode2 = -1, opcode11 = -1, opcode12 = -1, mask = -1, MASK = -1;
    int r1 = -1, r2 = -1, R1 = -1, R2 = -1, rs = -1, rx = -1;
    int imm = -1, IMM = -1, d = -1, dh = -1, label_off = -1, const_ref_num = -1, label_ref_num = -1;
    int n, len = 0, v, reg;
    int switch_table_addr_p = FALSE;

    for (p = insn_str; (ch = *p) != '\0' && ch != ';'; p++) {
      if ((ch = *p) == 0 || ch == ';') break;
      if ((n = read_curr_hex (&p, &v)) > 0) {
        gen_assert (n == 4 || n == 2);
        len = 4;
        if (n == 4) {
          opcode2 = v;
          ch2 = *++p;
          if (ch2 == '*') { /* sil */
            len = 6;
          } else {
            p--;
          }
        } else {
          opcode1 = v;
          ch2 = *++p;
          if (ch2 != ':') { /* rr, rx, rs */
            if (ch2 == '*') {
              len = 2;
            } else {
              p--;
            }
          } else {
            p++;
            n = read_curr_hex (&p, &v);
            gen_assert (n == 1 || n == 2);
            if (n == 1) {
              ch2 = *++p;
              if (ch2 == '*') { /* ril */
                len = 6;
              } else {
                p--;
              }
              opcode11 = v;
            } else {
              len = 6;
              opcode12 = v;
            }
          }
        }
        continue;
      }
      switch ((start_ch = ch = *p)) {
      case ' ':
      case '\t': break;
      case 'h':
      case 'H':
        ch2 = *++p;
        if (ch2 == 's' || ch2 == 'x') {
          ch = ch2;
        } else {
          p--;
        }
        reg = read_dec (&p);
        gen_assert (reg >= 0 && reg <= F15_HARD_REG);
        if (reg >= F0_HARD_REG) reg -= F0_HARD_REG;
        goto set_reg;
      case 'r':
      case 'R':
      case 'x':
      case 's':
      case 'n':
        ch2 = *++p;
        gen_assert ('0' <= ch2 && ch2 <= '2' && ch2 - '0' < (int) nops);
        op = insn->ops[ch2 - '0'];
        gen_assert (op.mode == MIR_OP_VAR);
        reg = op.u.var;
        gen_assert (ch != 'n' || reg >= F0_HARD_REG);
        if (start_ch == 'n') reg += 2;
        gen_assert (reg <= F15_HARD_REG);
        if (reg >= F0_HARD_REG) reg -= F0_HARD_REG;
      set_reg:
        if (ch == 'r' || ch == 'h' || ch == 'n') {
          if (opcode2 < 0) {
            gen_assert (r1 < 0);
            r1 = reg;
          } else {
            gen_assert (R1 < 0);
            R1 = reg;
          }
        } else if (ch == 'R' || ch == 'H') {
          if (opcode2 < 0) {
            gen_assert (r2 < 0);
            r2 = reg;
          } else {
            gen_assert (R2 < 0);
            R2 = reg;
          }
        } else if (ch == 'x') {
          gen_assert (rx < 0 && reg != 0);
          rx = reg;
        } else {
          gen_assert (ch == 's' && rs < 0 && reg != 0);
          rs = reg;
        }
        break;
      case 'm':
        ch2 = *++p;
        if (ch2 == 'a') { /* mask */
          if (opcode2 < 0) {
            gen_assert (mask < 0);
            mask = read_dec (&p);
          } else {
            gen_assert (MASK < 0);
            MASK = read_dec (&p);
          }
        } else if (ch2 == 'd' || ch2 == 'D') { /* displacement from immediate */
          gen_assert (d < 0 && dh < 0);
          if (ch2 != 'd' || (d = read_dec (&p)) < 0) {
            u = get_imm (gen_ctx, insn);
            d = u & 0xfff;
            dh = ((int64_t) u >> 12) & 0xff;
            if (dh == 0) dh = -1;
          }
        } else {
          if (ch2 != 'n') p--;
          if (insn->ops[0].mode == MIR_OP_VAR_MEM) {
            op = insn->ops[0];
          } else if (nops >= 2 && insn->ops[1].mode == MIR_OP_VAR_MEM) {
            op = insn->ops[1];
          } else if (nops >= 3 && insn->ops[2].mode == MIR_OP_VAR_MEM) {
            op = insn->ops[2];
          } else {
            gen_assert (FALSE);
          }
          gen_assert (rs < 0 && rx < 0);
          if (ch2 == 'n') op.u.var_mem.disp += 8;
          gen_assert (op.u.var_mem.index == MIR_NON_VAR || op.u.var_mem.scale == 1);
          if (op.u.var_mem.base == MIR_NON_VAR) {
            if (op.u.var_mem.index != MIR_NON_VAR) rs = op.u.var_mem.index;
          } else {
            rs = op.u.var_mem.base;
            if (op.u.var_mem.index != MIR_NON_VAR) rx = op.u.var_mem.index;
          }
          gen_assert (d < 0 && dh < 0);
          d = op.u.var_mem.disp & 0xfff;
          dh = (op.u.var_mem.disp >> 12) & 0xff;
          if (dh == 0) dh = -1;
        }
        break;
      case 'i':
        gen_assert (imm < 0);
        if ((imm = read_dec (&p)) >= 0) {
        } else {
          u = get_imm (gen_ctx, insn);
          imm = u & 0xffff;
        }
        break;
      case 'u':
        gen_assert (imm < 0);
        u = get_imm (gen_ctx, insn);
        ch2 = *++p;
        if (ch2 == 'a') {
          imm = (u + 7) / 8 * 8;
        } else {
          gen_assert ('0' <= ch2 && ch2 <= '3');
          imm = (u >> (ch2 - '0') * 16) & 0xffff;
        }
        break;
      case 'j':
        gen_assert (IMM < 0);
        u = get_imm (gen_ctx, insn);
        IMM = u & 0xffff;
        break;
      case 'I': {
        uint64_t imm_val;

        ch2 = *++p;
        gen_assert (ch2 == 'a');
        gen_assert (const_ref_num < 0);
        imm_val = get_imm (gen_ctx, insn);
        const_ref_num = setup_imm_addr (gen_ctx, imm_val);
        break;
      }
      case 'S':
        ch2 = *++p;
        gen_assert (ch2 == 'd' || ch2 == 'D');
        gen_assert (d < 0 && dh < 0);
        u = read_dec (&p);
        d = u & 0xfff;
        dh = ((int64_t) u >> 12) & 0xff;
        gen_assert (ch2 == 'D' || dh == 0);
        if (dh == 0) dh = -1;
        break;
      case 'l':
        label_off = read_dec (&p);
        gen_assert (label_off % 2 == 0 && label_off >= 0);
        label_off /= 2;
        break;
      case 'L':
        op = insn->ops[insn->code != MIR_CALL && insn->code != MIR_LADDR ? 0 : 1];
        gen_assert (op.mode == MIR_OP_LABEL);
        lr.abs_addr_p = FALSE;
        lr.label_val_disp = 0;
        if (jump_addrs == NULL)
          lr.u.label = op.u.label;
        else
          lr.u.jump_addr = jump_addrs[0];
        label_ref_num = VARR_LENGTH (label_ref_t, label_refs);
        VARR_PUSH (label_ref_t, label_refs, lr);
        break;
      case 'T':
        gen_assert (!switch_table_addr_p && switch_table_addr_insn_start < 0);
        switch_table_addr_p = TRUE;
        break;
      case 'Q': {
        int64_t size = S390X_STACK_HEADER_SIZE + param_save_area_size + blk_ld_value_save_area_size;
        gen_assert (d < 0 && dh < 0 && int20_p (size));
        d = (size) &0xfff;
        dh = ((size) >> 12) & 0xff;
        if (dh == 0) dh = -1;
        break;
      }
      default: gen_assert (FALSE);
      }
    }

    if (opcode1 >= 0) {
      gen_assert (opcode1 < 256);
      set_insn_field (&binsn, opcode1, 0, 8);
      check_and_set_mask (&binsn_mask, 0xff, 0, 8);
    }
    if (opcode2 >= 0) {
      gen_assert (opcode2 < (1 << 16));
      set_insn_field (&binsn, opcode2, 0, 16);
      check_and_set_mask (&binsn_mask, 0xffff, 0, 16);
    }
    if (opcode11 >= 0) {
      gen_assert (opcode11 < 16);
      set_insn_field (&binsn, opcode11, 12, 4);
      check_and_set_mask (&binsn_mask, 0xf, 12, 4);
    }
    if (opcode12 >= 0) {
      gen_assert (opcode12 < 256);
      set_insn_field (&binsn, opcode12, 40, 8);
      check_and_set_mask (&binsn_mask, 0xff, 40, 8);
    }
    if (r1 >= 0) {
      gen_assert (r1 < 16);
      set_insn_field (&binsn, r1, 8, 4);
      check_and_set_mask (&binsn_mask, 0xf, 8, 4);
    }
    if (R1 >= 0) {
      gen_assert (R1 < 16);
      set_insn_field (&binsn, R1, 24, 4);
      check_and_set_mask (&binsn_mask, 0xf, 24, 4);
    }
    if (r2 >= 0) {
      gen_assert (r2 < 16);
      set_insn_field (&binsn, r2, 12, 4);
      check_and_set_mask (&binsn_mask, 0xf, 12, 4);
    }
    if (R2 >= 0) {
      gen_assert (R2 < 16);
      set_insn_field (&binsn, R2, 28, 4);
      check_and_set_mask (&binsn_mask, 0xf, 28, 4);
    }
    if (rs >= 0) {
      gen_assert (rs < 16);
      set_insn_field (&binsn, rs, 16, 4);
      check_and_set_mask (&binsn_mask, 0xf, 16, 4);
    }
    if (rx >= 0) {
      gen_assert (rx < 16);
      set_insn_field (&binsn, rx, 12, 4);
      check_and_set_mask (&binsn_mask, 0xf, 12, 4);
    }
    if (d >= 0) {
      gen_assert (d < (1 << 12));
      set_insn_field (&binsn, d, 20, 12);
      check_and_set_mask (&binsn_mask, 0xfff, 20, 12);
    }
    if (dh >= 0) {
      gen_assert (dh < (1 << 8));
      set_insn_field (&binsn, dh, 32, 8);
      check_and_set_mask (&binsn_mask, 0xff, 32, 8);
    }
    if (imm >= 0) {
      gen_assert (imm < (1 << 16));
      set_insn_field (&binsn, imm, 16, 16);
      check_and_set_mask (&binsn_mask, 0xffff, 16, 16);
    }
    if (IMM >= 0) {
      gen_assert (IMM < (1 << 16));
      set_insn_field (&binsn, IMM, 32, 16);
      check_and_set_mask (&binsn_mask, 0xffff, 32, 16);
    }
    if (mask >= 0) {
      gen_assert (mask < 16);
      set_insn_field (&binsn, mask, 8, 4);
      check_and_set_mask (&binsn_mask, 0xf, 8, 4);
    }
    if (MASK >= 0) {
      gen_assert (MASK < 16);
      set_insn_field (&binsn, MASK, 16, 4);
      check_and_set_mask (&binsn_mask, 0xf, 16, 4);
    }
    if (label_off >= 0) {
      gen_assert (label_off < (1 << 16));
      set_insn_field (&binsn, label_off, 16, 16);
      check_and_set_mask (&binsn_mask, 0xffff, 16, 16);
    }
    if (const_ref_num >= 0) VARR_ADDR (const_ref_t, const_refs)
    [const_ref_num].insn_pc = VARR_LENGTH (uint8_t, result_code);
    if (label_ref_num >= 0) VARR_ADDR (label_ref_t, label_refs)
    [label_ref_num].label_val_disp = VARR_LENGTH (uint8_t, result_code);

    if (switch_table_addr_p) switch_table_addr_insn_start = VARR_LENGTH (uint8_t, result_code);
    VARR_PUSH_ARR (uint8_t, result_code, (uint8_t *) &binsn, len); /* output the machine insn */
    if (const_ref_num >= 0) VARR_ADDR (const_ref_t, const_refs)
    [const_ref_num].next_insn_pc = VARR_LENGTH (uint8_t, result_code);

    if (*p == 0) break;
  }

  if (switch_table_addr_insn_start < 0) return;
  while (VARR_LENGTH (uint8_t, result_code) % 8 != 0) put_arr (gen_ctx, &nop_binsn, 2);
  /* pc offset of insn with T plus 8 bytes of insns after T: see switch */
  offset = (VARR_LENGTH (uint8_t, result_code) - switch_table_addr_insn_start);
  *(uint32_t *) (VARR_ADDR (uint8_t, result_code) + switch_table_addr_insn_start + 2) |= offset / 2;
  gen_assert (insn->code == MIR_SWITCH);
  for (size_t i = 1; i < insn->nops; i++) {
    gen_assert (insn->ops[i].mode == MIR_OP_LABEL);
    lr.abs_addr_p = TRUE;
    lr.label_val_disp = VARR_LENGTH (uint8_t, result_code);
    if (jump_addrs == NULL)
      lr.u.label = insn->ops[i].u.label;
    else
      lr.u.jump_addr = jump_addrs[i - 1];
    VARR_PUSH (label_ref_t, label_refs, lr);
    /* reserve space for absolute label address: */
    VARR_PUSH_ARR (uint8_t, result_code, (uint8_t *) &zero64, sizeof (zero64));
  }
}

static int target_memory_ok_p (gen_ctx_t gen_ctx MIR_UNUSED, MIR_op_t *op_ref) {
  if (op_ref->mode != MIR_OP_VAR_MEM) return FALSE;
  if (((op_ref->u.var_mem.type != MIR_T_U64 && op_ref->u.var_mem.type != MIR_T_U64)
       || op_ref->u.var_mem.base == MIR_NON_VAR || op_ref->u.var_mem.index == MIR_NON_VAR)
      && (op_ref->u.var_mem.index == MIR_NON_VAR || op_ref->u.var_mem.scale == 1)
      && int20_p (op_ref->u.var_mem.disp)
      && (op_ref->u.var_mem.type != MIR_T_LD || int20_p (op_ref->u.var_mem.disp + 8)))
    return TRUE;
  return FALSE;
}

static int target_insn_ok_p (gen_ctx_t gen_ctx, MIR_insn_t insn) {
  return find_insn_pattern_replacement (gen_ctx, insn) != NULL;
}

static void add_consts (gen_ctx_t gen_ctx) {
  while (VARR_LENGTH (uint8_t, result_code) % 16 != 0) /* Align the pool */
    VARR_PUSH (uint8_t, result_code, 0);
  for (size_t i = 0; i < VARR_LENGTH (const_ref_t, const_refs); i++) { /* Add pool constants */
    const_ref_t cr = VARR_GET (const_ref_t, const_refs, i);
    int64_t offset = VARR_LENGTH (uint8_t, result_code) - cr.insn_pc;
    uint64_t zero64 = 0;

    gen_assert (offset > 0 && offset % 2 == 0);
    offset /= 2;
    gen_assert ((offset >> 31) == 0);
    set_int32 (VARR_ADDR (uint8_t, result_code) + cr.insn_pc + 2 /* start disp in LALR */, offset);
    put_arr (gen_ctx, &VARR_ADDR (uint64_t, const_pool)[cr.const_num], 8);
    put_arr (gen_ctx, &zero64, sizeof (zero64)); /* keep 16 bytes align */
  }
}

static void target_split_insns (gen_ctx_t gen_ctx MIR_UNUSED) {}

static uint8_t *target_translate (gen_ctx_t gen_ctx, size_t *len) {
  MIR_context_t ctx = gen_ctx->ctx;
  size_t i;
  MIR_insn_t insn, old_insn;
  const char *replacement;

  gen_assert (curr_func_item->item_type == MIR_func_item);
  VARR_TRUNC (uint8_t, result_code, 0);
  VARR_TRUNC (uint64_t, const_pool, 0);
  VARR_TRUNC (const_ref_t, const_refs, 0);
  VARR_TRUNC (label_ref_t, label_refs, 0);
  VARR_TRUNC (uint64_t, abs_address_locs, 0);
  for (insn = DLIST_HEAD (MIR_insn_t, curr_func_item->u.func->insns); insn != NULL;
       insn = DLIST_NEXT (MIR_insn_t, insn)) {
    MIR_insn_code_t code = insn->code;

    /* ???split/change */
    if ((code == MIR_RSH || code == MIR_LSH || code == MIR_URSH || code == MIR_RSHS
         || code == MIR_LSHS || code == MIR_URSHS)
        && (insn->ops[2].mode == MIR_OP_INT || insn->ops[2].mode == MIR_OP_UINT)) {
      if (insn->ops[2].u.i == 0) {
        gen_mov (gen_ctx, insn, MIR_MOV, insn->ops[0], insn->ops[1]);
        old_insn = insn;
        insn = DLIST_NEXT (MIR_insn_t, insn);
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
    if (insn->code == MIR_LABEL) {
      set_label_disp (gen_ctx, insn, VARR_LENGTH (uint8_t, result_code));
    } else if (insn->code != MIR_USE) {
      replacement = find_insn_pattern_replacement (gen_ctx, insn);
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
    } else { /* 32-bit relative address */
      int64_t offset = (int64_t) get_label_disp (gen_ctx, lr.u.label) - (int64_t) lr.label_val_disp;
      gen_assert (offset % 2 == 0);
      offset /= 2;
      gen_assert (((offset < 0 ? -offset : offset) & ~(int64_t) 0x7fffffff) == 0);
      *(uint32_t *) (VARR_ADDR (uint8_t, result_code) + lr.label_val_disp + 2)
        |= (offset & 0xffffffff);
    }
  }
  add_consts (gen_ctx);
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
  VARR_TRUNC (uint8_t, result_code, 0);
  VARR_TRUNC (const_ref_t, const_refs, 0);
  VARR_TRUNC (label_ref_t, label_refs, 0);
  VARR_TRUNC (uint64_t, abs_address_locs, 0);
}

static void target_bb_insn_translate (gen_ctx_t gen_ctx, MIR_insn_t insn, void **jump_addrs) {
  const char *replacement;
  if (insn->code == MIR_LABEL) return;
  replacement = find_insn_pattern_replacement (gen_ctx, insn);
  gen_assert (replacement != NULL);
  out_insn (gen_ctx, insn, replacement, jump_addrs);
}

static void target_output_jump (gen_ctx_t gen_ctx, void **jump_addrs) {
  out_insn (gen_ctx, temp_jump, temp_jump_replacement, jump_addrs);
}

static uint8_t *target_bb_translate_finish (gen_ctx_t gen_ctx, size_t *len) {
  add_consts (gen_ctx);
  *len = VARR_LENGTH (uint8_t, result_code);
  return VARR_ADDR (uint8_t, result_code);
}

static void setup_rel (gen_ctx_t gen_ctx, label_ref_t *lr, uint8_t *base, void *addr) {
  MIR_context_t ctx = gen_ctx->ctx;
  int64_t offset = (int64_t) addr - (int64_t) (base + lr->label_val_disp);
  int32_t rel32 = offset;

  gen_assert ((offset & 0x1) == 0);
  offset >>= 1;
  gen_assert (((offset < 0 ? -offset : offset) & ~(int64_t) 0x7fffffff) == 0);
  /* check max 32-bit offset with possible branch conversion (see offset): */
  if (lr->abs_addr_p || ((offset < 0 ? -offset : offset) & ~(int64_t) 0x7fffffff) != 0) {
    fprintf (stderr, "too big offset (%lld) in setup_rel", (long long) offset);
    exit (1);
  }
  /* ??? thread safe: */
  uint32_t *insn_ptr = (uint32_t *) (base + lr->label_val_disp);
  rel32 = offset & 0xffffffff;
  _MIR_change_code (ctx, (uint8_t *) insn_ptr + 2, (uint8_t *) &rel32, 4);
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
  VARR_CREATE (uint64_t, const_pool, alloc, 0);
  VARR_CREATE (const_ref_t, const_refs, alloc, 0);
  VARR_CREATE (label_ref_t, label_refs, alloc, 0);
  VARR_CREATE (uint64_t, abs_address_locs, alloc, 0);
  VARR_CREATE (MIR_code_reloc_t, relocs, alloc, 0);
  VARR_CREATE (uint64_t, ld_addr_regs, alloc, 0);
  patterns_init (gen_ctx);
  temp_jump = MIR_new_insn (ctx, MIR_JMP, MIR_new_label_op (ctx, NULL));
  temp_jump_replacement = find_insn_pattern_replacement (gen_ctx, temp_jump);
}

static void target_finish (gen_ctx_t gen_ctx) {
  MIR_alloc_t alloc = gen_alloc (gen_ctx);
  patterns_finish (gen_ctx);
  _MIR_free_insn (gen_ctx->ctx, temp_jump);
  VARR_DESTROY (uint8_t, result_code);
  VARR_DESTROY (uint64_t, const_pool);
  VARR_DESTROY (const_ref_t, const_refs);
  VARR_DESTROY (label_ref_t, label_refs);
  VARR_DESTROY (uint64_t, abs_address_locs);
  VARR_DESTROY (MIR_code_reloc_t, relocs);
  VARR_DESTROY (uint64_t, ld_addr_regs);
  MIR_free (alloc, gen_ctx->target_ctx);
  gen_ctx->target_ctx = NULL;
}
