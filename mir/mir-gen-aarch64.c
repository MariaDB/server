/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

static void fancy_abort (int code) {
  if (!code) abort ();
}
#undef gen_assert
#define gen_assert(c) fancy_abort (c)

#include <limits.h>

#include "mir-aarch64.h"

static const MIR_reg_t FP_HARD_REG = R29_HARD_REG;
static const MIR_reg_t LINK_HARD_REG = R30_HARD_REG;

static inline MIR_reg_t target_nth_loc (MIR_reg_t loc, MIR_type_t type MIR_UNUSED, int n) {
  return loc + n;
}

static inline int target_call_used_hard_reg_p (MIR_reg_t hard_reg, MIR_type_t type) {
  assert (hard_reg <= MAX_HARD_REG);
  if (hard_reg <= SP_HARD_REG) return !(hard_reg >= R19_HARD_REG && hard_reg <= R28_HARD_REG);
  return type == MIR_T_LD || !(hard_reg >= V8_HARD_REG && hard_reg <= V15_HARD_REG);
}

/* Stack layout (sp refers to the last reserved stack slot address)
   from higher address to lower address memory:

   | ...           |  prev func stack (start aligned to 16 bytes)
   |---------------|
   | gr save area  |  64 bytes optional area for vararg func integer reg save area (absent for
   APPLE)
   |---------------|
   | vr save area  |  128 bytes optional area for vararg func fp reg save area (absent for APPLE)
   |---------------|
   | saved regs    |  callee saved regs used in the func (known only after RA), rounded 16 bytes
   |---------------|
   | slots assigned|  can be absent for small functions (known only after RA), rounded 16 bytes
   |   to pseudos  |
   |---------------|
   |   previous    |  16-bytes setup in prolog, used only for varag func or args passed on stack
   | stack start   |  to move args and to setup va_start on machinize pass
   |---------------|
   | LR            |  sp before prologue and after saving LR = start sp
   |---------------|
   | old FP        |  frame pointer for previous func stack frame; new FP refers for here
   |               |  it has lowest address as 12-bit offsets are only positive
   |---------------|
   |  small aggr   |
   |  save area    |  optional
   |---------------|
   | alloca areas  |  optional
   |---------------|
   | slots for     |  dynamically allocated/deallocated by caller
   |  passing args |

   size of slots and saved regs is multiple of 16 bytes

 */

#if !defined(__APPLE__)
static const int int_reg_save_area_size = 8 * 8;
static const int reg_save_area_size = 8 * 8 + 8 * 16;
#endif

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

static MIR_reg_t get_arg_reg (MIR_type_t arg_type, size_t *int_arg_num, size_t *fp_arg_num,
                              MIR_insn_code_t *mov_code) {
  MIR_reg_t arg_reg;

  if (arg_type == MIR_T_F || arg_type == MIR_T_D || arg_type == MIR_T_LD) {
    switch (*fp_arg_num) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7: arg_reg = V0_HARD_REG + *fp_arg_num; break;
    default: arg_reg = MIR_NON_VAR; break;
    }
    (*fp_arg_num)++;
    *mov_code = arg_type == MIR_T_F ? MIR_FMOV : arg_type == MIR_T_D ? MIR_DMOV : MIR_LDMOV;
  } else { /* including BLK, RBLK: */
    switch (*int_arg_num) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7: arg_reg = R0_HARD_REG + *int_arg_num; break;
    default: arg_reg = MIR_NON_VAR; break;
    }
    (*int_arg_num)++;
    *mov_code = MIR_MOV;
  }
  return arg_reg;
}

static void mir_blk_mov (uint64_t *to, uint64_t *from, uint64_t nwords) {
  for (; nwords > 0; nwords--) *to++ = *from++;
}

static MIR_insn_t gen_mov (gen_ctx_t gen_ctx, MIR_insn_t anchor, MIR_insn_code_t code,
                           MIR_op_t dst_op, MIR_op_t src_op) {
  MIR_insn_t insn = MIR_new_insn (gen_ctx->ctx, code, dst_op, src_op);
  gen_add_insn_before (gen_ctx, anchor, insn);
  return insn;
}

static MIR_reg_t target_get_stack_slot_base_reg (gen_ctx_t gen_ctx MIR_UNUSED) {
  return FP_HARD_REG;
}

static int target_valid_mem_offset_p (gen_ctx_t gen_ctx, MIR_type_t type, MIR_disp_t offset);

static MIR_op_t new_mem_op (gen_ctx_t gen_ctx, MIR_insn_t anchor, MIR_type_t type, MIR_disp_t disp,
                            MIR_reg_t base) {
  MIR_context_t ctx = gen_ctx->ctx;
  if (target_valid_mem_offset_p (gen_ctx, type, disp))
    return _MIR_new_var_mem_op (ctx, type, disp, base, MIR_NON_VAR, 1);
  MIR_reg_t temp_reg = gen_new_temp_reg (gen_ctx, MIR_T_I64, curr_func_item->u.func);
  MIR_op_t temp_reg_op = _MIR_new_var_op (ctx, temp_reg);
  gen_mov (gen_ctx, anchor, MIR_MOV, temp_reg_op, MIR_new_int_op (ctx, disp));
  gen_add_insn_before (gen_ctx, anchor,
                       MIR_new_insn (ctx, MIR_ADD, temp_reg_op, temp_reg_op,
                                     _MIR_new_var_op (ctx, base)));
  return _MIR_new_var_mem_op (ctx, type, 0, temp_reg, MIR_NON_VAR, 1);
}

static MIR_op_t get_new_hard_reg_mem_op (gen_ctx_t gen_ctx, MIR_type_t type, MIR_disp_t disp,
                                         MIR_reg_t base, MIR_insn_t *insn1, MIR_insn_t *insn2) {
  MIR_context_t ctx = gen_ctx->ctx;
  *insn1 = *insn2 = NULL;
  if (target_valid_mem_offset_p (gen_ctx, type, disp))
    return _MIR_new_var_mem_op (ctx, type, disp, base, MIR_NON_VAR, 1);
  MIR_op_t temp_reg_op = _MIR_new_var_op (ctx, TEMP_INT_HARD_REG2);
  *insn1 = MIR_new_insn (ctx, MIR_MOV, temp_reg_op, MIR_new_int_op (ctx, disp));
  *insn2 = MIR_new_insn (ctx, MIR_ADD, temp_reg_op, temp_reg_op, _MIR_new_var_op (ctx, base));
  return _MIR_new_var_mem_op (ctx, type, 0, TEMP_INT_HARD_REG2, MIR_NON_VAR, 1);
}

static MIR_op_t new_hard_reg_mem_op (gen_ctx_t gen_ctx, MIR_insn_t anchor, MIR_type_t type,
                                     MIR_disp_t disp, MIR_reg_t base) {
  MIR_insn_t insn1, insn2;
  MIR_op_t op = get_new_hard_reg_mem_op (gen_ctx, type, disp, base, &insn1, &insn2);
  if (insn1 != NULL) gen_add_insn_before (gen_ctx, anchor, insn1);
  if (insn2 != NULL) gen_add_insn_before (gen_ctx, anchor, insn2);
  return op;
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
               new_mem_op (gen_ctx, anchor, MIR_T_I64, from_disp, from_base_reg));
      gen_mov (gen_ctx, anchor, MIR_MOV,
               new_hard_reg_mem_op (gen_ctx, anchor, MIR_T_I64, to_disp, to_base_hard_reg),
               treg_op);
    }
    return;
  }
  treg_op2 = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
  treg_op3 = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
  /* Save arg regs: */
  if (save_regs > 0)
    gen_mov (gen_ctx, anchor, MIR_MOV, treg_op, _MIR_new_var_op (ctx, R0_HARD_REG));
  if (save_regs > 1)
    gen_mov (gen_ctx, anchor, MIR_MOV, treg_op2, _MIR_new_var_op (ctx, R1_HARD_REG));
  if (save_regs > 2)
    gen_mov (gen_ctx, anchor, MIR_MOV, treg_op3, _MIR_new_var_op (ctx, R2_HARD_REG));
  /* call blk move: */
  proto_item = _MIR_builtin_proto (ctx, curr_func_item->module, BLK_MOV_P, 0, NULL, 3, MIR_T_I64,
                                   "to", MIR_T_I64, "from", MIR_T_I64, "nwords");
  func_import_item = _MIR_builtin_func (ctx, curr_func_item->module, BLK_MOV, mir_blk_mov);
  freg_op = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
  new_insn = MIR_new_insn (ctx, MIR_MOV, freg_op, MIR_new_ref_op (ctx, func_import_item));
  gen_add_insn_before (gen_ctx, anchor, new_insn);
  gen_add_insn_before (gen_ctx, anchor,
                       MIR_new_insn (gen_ctx->ctx, MIR_ADD, _MIR_new_var_op (ctx, R0_HARD_REG),
                                     _MIR_new_var_op (ctx, to_base_hard_reg),
                                     MIR_new_int_op (ctx, to_disp)));
  gen_add_insn_before (gen_ctx, anchor,
                       MIR_new_insn (gen_ctx->ctx, MIR_ADD, _MIR_new_var_op (ctx, R1_HARD_REG),
                                     _MIR_new_var_op (ctx, from_base_reg),
                                     MIR_new_int_op (ctx, from_disp)));
  gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, R2_HARD_REG),
           MIR_new_int_op (ctx, qwords));
  ops[0] = MIR_new_ref_op (ctx, proto_item);
  ops[1] = freg_op;
  ops[2] = _MIR_new_var_op (ctx, R0_HARD_REG);
  ops[3] = _MIR_new_var_op (ctx, R1_HARD_REG);
  ops[4] = _MIR_new_var_op (ctx, R2_HARD_REG);
  new_insn = MIR_new_insn_arr (ctx, MIR_CALL, 5, ops);
  gen_add_insn_before (gen_ctx, anchor, new_insn);
  /* Restore arg regs: */
  if (save_regs > 0)
    gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, R0_HARD_REG), treg_op);
  if (save_regs > 1)
    gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, R1_HARD_REG), treg_op2);
  if (save_regs > 2)
    gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, R2_HARD_REG), treg_op3);
}

static void machinize_call (gen_ctx_t gen_ctx, MIR_insn_t call_insn) {
  MIR_context_t ctx = gen_ctx->ctx;
  MIR_func_t func = curr_func_item->u.func;
  MIR_proto_t proto = call_insn->ops[0].u.ref->u.proto;
  size_t nargs, nops = MIR_insn_nops (ctx, call_insn), start = proto->nres + 2;
  size_t int_arg_num = 0, fp_arg_num = 0, mem_size = 0, blk_offset = 0, qwords;
  MIR_type_t type, mem_type;
  MIR_op_mode_t mode;
  MIR_var_t *arg_vars = NULL;
  MIR_reg_t arg_reg;
  MIR_op_t arg_op, temp_op, arg_reg_op, ret_reg_op, mem_op;
  MIR_insn_code_t new_insn_code, ext_code;
  MIR_insn_t new_insn, prev_insn, next_insn, ext_insn, insn1, insn2;
  MIR_insn_t prev_call_insn = DLIST_PREV (MIR_insn_t, call_insn);
  MIR_insn_t curr_prev_call_insn = prev_call_insn;
  uint32_t n_iregs, n_vregs;

  if (call_insn->code == MIR_INLINE) call_insn->code = MIR_CALL;
  if (proto->args == NULL) {
    nargs = 0;
  } else {
    gen_assert (nops >= VARR_LENGTH (MIR_var_t, proto->args)
                && (proto->vararg_p || nops - start == VARR_LENGTH (MIR_var_t, proto->args)));
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
  for (size_t i = start; i < nops; i++) { /* calculate offset for blk params */
    if (i - start < nargs) {
      type = arg_vars[i - start].type;
    } else if (call_insn->ops[i].mode == MIR_OP_VAR_MEM) {
      type = call_insn->ops[i].u.var_mem.type;
      gen_assert (MIR_all_blk_type_p (type));
    } else {
      mode = call_insn->ops[i].value_mode;  // ??? smaller ints
      gen_assert (mode == MIR_OP_INT || mode == MIR_OP_UINT || mode == MIR_OP_FLOAT
                  || mode == MIR_OP_DOUBLE || mode == MIR_OP_LDOUBLE);
      if (mode == MIR_OP_FLOAT)
        (*MIR_get_error_func (ctx)) (MIR_call_op_error,
                                     "passing float variadic arg (should be passed as double)");
      if (mode == MIR_OP_LDOUBLE && __SIZEOF_LONG_DOUBLE__ == 8) mode = MIR_OP_DOUBLE;
      type = mode == MIR_OP_DOUBLE ? MIR_T_D : mode == MIR_OP_LDOUBLE ? MIR_T_LD : MIR_T_I64;
    }
    gen_assert (!MIR_all_blk_type_p (type) || call_insn->ops[i].mode == MIR_OP_VAR_MEM);
    if (type == MIR_T_RBLK && i == start) continue; /* hidden arg */
#if defined(__APPLE__)                              /* all varargs are passed on stack */
    if (i - start == nargs) int_arg_num = fp_arg_num = 8;
#endif
    if (MIR_blk_type_p (type) && (qwords = (call_insn->ops[i].u.var_mem.disp + 7) / 8) <= 2) {
      if (int_arg_num + qwords > 8) blk_offset += qwords * 8;
      int_arg_num += qwords;
    } else if (get_arg_reg (type, &int_arg_num, &fp_arg_num, &new_insn_code) == MIR_NON_VAR) {
      if (type == MIR_T_LD && __SIZEOF_LONG_DOUBLE__ == 16 && blk_offset % 16 != 0)
        blk_offset = (blk_offset + 15) / 16 * 16;
      blk_offset += type == MIR_T_LD && __SIZEOF_LONG_DOUBLE__ == 16 ? 16 : 8;
    }
  }
  blk_offset = (blk_offset + 15) / 16 * 16;
  int_arg_num = fp_arg_num = 0;
  for (size_t i = start; i < nops; i++) {
#if defined(__APPLE__) /* all varargs are passed on stack */
    if (i - start == nargs) int_arg_num = fp_arg_num = 8;
#endif
    arg_op = call_insn->ops[i];
    gen_assert (arg_op.mode == MIR_OP_VAR
                || (arg_op.mode == MIR_OP_VAR_MEM && MIR_all_blk_type_p (arg_op.u.var_mem.type)));
    if (i - start < nargs) {
      type = arg_vars[i - start].type;
    } else if (call_insn->ops[i].mode == MIR_OP_VAR_MEM) {
      type = call_insn->ops[i].u.var_mem.type;
      gen_assert (MIR_all_blk_type_p (type));
    } else {
      mode = call_insn->ops[i].value_mode;  // ??? smaller ints
      if (mode == MIR_OP_LDOUBLE && __SIZEOF_LONG_DOUBLE__ == 8) mode = MIR_OP_DOUBLE;
      type = mode == MIR_OP_DOUBLE ? MIR_T_D : mode == MIR_OP_LDOUBLE ? MIR_T_LD : MIR_T_I64;
    }
    ext_insn = NULL;
    if ((ext_code = get_ext_code (type)) != MIR_INVALID_INSN) { /* extend arg if necessary */
      temp_op = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
      ext_insn = MIR_new_insn (ctx, ext_code, temp_op, arg_op);
      call_insn->ops[i] = arg_op = temp_op;
    }
    gen_assert (!MIR_all_blk_type_p (type)
                || (arg_op.mode == MIR_OP_VAR_MEM && arg_op.u.var_mem.disp >= 0
                    && arg_op.u.var_mem.index == MIR_NON_VAR));
    if (type == MIR_T_RBLK && i == start) { /* hidden arg */
      arg_reg_op = _MIR_new_var_op (ctx, R8_HARD_REG);
      gen_mov (gen_ctx, call_insn, MIR_MOV, arg_reg_op,
               _MIR_new_var_op (ctx, arg_op.u.var_mem.base));
      call_insn->ops[i] = arg_reg_op;
      continue;
    } else if (MIR_blk_type_p (type)) {
      qwords = (arg_op.u.var_mem.disp + 7) / 8;
      if (qwords <= 2) {
        arg_reg = R0_HARD_REG + int_arg_num;
        if (int_arg_num + qwords <= 8) {
          /* A trick to keep arg regs live: */
          call_insn->ops[i] = _MIR_new_var_mem_op (ctx, MIR_T_UNDEF, 0, int_arg_num,
                                                   qwords < 2 ? MIR_NON_VAR : int_arg_num + 1, 1);
          if (qwords == 0) continue;
          new_insn = MIR_new_insn (ctx, MIR_MOV, _MIR_new_var_op (ctx, R0_HARD_REG + int_arg_num++),
                                   _MIR_new_var_mem_op (ctx, MIR_T_I64, 0, arg_op.u.var_mem.base,
                                                        MIR_NON_VAR, 1));
          gen_add_insn_before (gen_ctx, call_insn, new_insn);
          if (qwords == 2) {
            new_insn
              = MIR_new_insn (ctx, MIR_MOV, _MIR_new_var_op (ctx, R0_HARD_REG + int_arg_num++),
                              _MIR_new_var_mem_op (ctx, MIR_T_I64, 8, arg_op.u.var_mem.base,
                                                   MIR_NON_VAR, 1));
            gen_add_insn_before (gen_ctx, call_insn, new_insn);
          }
        } else { /* pass on stack w/o address: */
          gen_blk_mov (gen_ctx, call_insn, mem_size, SP_HARD_REG, 0, arg_op.u.var_mem.base, qwords,
                       int_arg_num);
          call_insn->ops[i]
            = _MIR_new_var_mem_op (ctx, MIR_T_UNDEF,
                                   mem_size, /* we don't care about valid mem disp here */
                                   SP_HARD_REG, MIR_NON_VAR, 1);
          mem_size += qwords * 8;
          blk_offset += qwords * 8;
          int_arg_num += qwords;
        }
        continue;
      }
      gen_blk_mov (gen_ctx, call_insn, blk_offset, SP_HARD_REG, 0, arg_op.u.var_mem.base, qwords,
                   int_arg_num);
      arg_op = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
      gen_assert (curr_prev_call_insn
                  != NULL); /* call_insn should not be 1st after simplification */
      new_insn = MIR_new_insn (gen_ctx->ctx, MIR_ADD, arg_op, _MIR_new_var_op (ctx, SP_HARD_REG),
                               MIR_new_int_op (ctx, blk_offset));
      gen_add_insn_after (gen_ctx, curr_prev_call_insn, new_insn);
      curr_prev_call_insn = DLIST_NEXT (MIR_insn_t, new_insn);
      blk_offset += qwords * 8;
    }
    if ((arg_reg = get_arg_reg (type, &int_arg_num, &fp_arg_num, &new_insn_code)) != MIR_NON_VAR) {
      /* put arguments to argument hard regs */
      if (ext_insn != NULL) gen_add_insn_before (gen_ctx, call_insn, ext_insn);
      arg_reg_op = _MIR_new_var_op (ctx, arg_reg);
      if (type != MIR_T_RBLK) {
        new_insn = MIR_new_insn (ctx, new_insn_code, arg_reg_op, arg_op);
      } else {
        assert (arg_op.mode == MIR_OP_VAR_MEM);
        new_insn
          = MIR_new_insn (ctx, MIR_MOV, arg_reg_op, _MIR_new_var_op (ctx, arg_op.u.var_mem.base));
        arg_reg_op
          = _MIR_new_var_mem_op (ctx, MIR_T_RBLK,
                                 arg_op.u.var_mem.disp, /* we don't care about valid disp here */
                                 arg_reg, MIR_NON_VAR, 1);
      }
      gen_add_insn_before (gen_ctx, call_insn, new_insn);
      call_insn->ops[i] = arg_reg_op;
    } else { /* put arguments on the stack */
      if (type == MIR_T_LD && __SIZEOF_LONG_DOUBLE__ == 16 && mem_size % 16 != 0)
        mem_size = (mem_size + 15) / 16 * 16;
      mem_type = type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD ? type : MIR_T_I64;
      new_insn_code = (type == MIR_T_F    ? MIR_FMOV
                       : type == MIR_T_D  ? MIR_DMOV
                       : type == MIR_T_LD ? MIR_LDMOV
                                          : MIR_MOV);
      mem_op = get_new_hard_reg_mem_op (gen_ctx, mem_type, mem_size, SP_HARD_REG, &insn1, &insn2);
      if (type != MIR_T_RBLK) {
        new_insn = MIR_new_insn (ctx, new_insn_code, mem_op, arg_op);
      } else {
        assert (arg_op.mode == MIR_OP_VAR_MEM);
        new_insn
          = MIR_new_insn (ctx, new_insn_code, mem_op, _MIR_new_var_op (ctx, arg_op.u.var_mem.base));
      }
      gen_assert (curr_prev_call_insn
                  != NULL); /* call_insn should not be 1st after simplification */
      MIR_insert_insn_after (ctx, curr_func_item, curr_prev_call_insn, new_insn);
      if (insn2 != NULL) MIR_insert_insn_after (ctx, curr_func_item, curr_prev_call_insn, insn2);
      if (insn1 != NULL) MIR_insert_insn_after (ctx, curr_func_item, curr_prev_call_insn, insn1);
      prev_insn = curr_prev_call_insn;
      next_insn = DLIST_NEXT (MIR_insn_t, new_insn);
      create_new_bb_insns (gen_ctx, prev_insn, next_insn, call_insn);
      call_insn->ops[i] = mem_op;
      mem_size += type == MIR_T_LD && __SIZEOF_LONG_DOUBLE__ == 16 ? 16 : 8;
      if (ext_insn != NULL) gen_add_insn_after (gen_ctx, curr_prev_call_insn, ext_insn);
      curr_prev_call_insn = new_insn;
    }
  }
  blk_offset = (blk_offset + 15) / 16 * 16;
  if (blk_offset != 0) mem_size = blk_offset;
  n_iregs = n_vregs = 0;
  for (size_t i = 0; i < proto->nres; i++) {
    int float_p;

    ret_reg_op = call_insn->ops[i + 2];
    gen_assert (ret_reg_op.mode == MIR_OP_VAR);
    type = proto->res_types[i];
    float_p = type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD;
    if (float_p && n_vregs < 8) {
      new_insn = MIR_new_insn (ctx,
                               type == MIR_T_F   ? MIR_FMOV
                               : type == MIR_T_D ? MIR_DMOV
                                                 : MIR_LDMOV,
                               ret_reg_op, _MIR_new_var_op (ctx, V0_HARD_REG + n_vregs));
      n_vregs++;
    } else if (!float_p && n_iregs < 8) {
      new_insn
        = MIR_new_insn (ctx, MIR_MOV, ret_reg_op, _MIR_new_var_op (ctx, R0_HARD_REG + n_iregs));
      n_iregs++;
    } else {
      (*MIR_get_error_func (ctx)) (MIR_ret_error,
                                   "aarch64 can not handle this combination of return values");
    }
    MIR_insert_insn_after (ctx, curr_func_item, call_insn, new_insn);
    call_insn->ops[i + 2] = new_insn->ops[1];
    if ((ext_code = get_ext_code (type)) != MIR_INVALID_INSN) {
      MIR_insert_insn_after (ctx, curr_func_item, new_insn,
                             MIR_new_insn (ctx, ext_code, ret_reg_op, ret_reg_op));
      new_insn = DLIST_NEXT (MIR_insn_t, new_insn);
    }
    create_new_bb_insns (gen_ctx, call_insn, DLIST_NEXT (MIR_insn_t, new_insn), call_insn);
  }
  if (mem_size != 0) {                    /* allocate/deallocate stack for args passed on stack */
    mem_size = (mem_size + 15) / 16 * 16; /* make it of several 16 bytes */
    new_insn = MIR_new_insn (ctx, MIR_SUB, _MIR_new_var_op (ctx, SP_HARD_REG),
                             _MIR_new_var_op (ctx, SP_HARD_REG), MIR_new_int_op (ctx, mem_size));
    MIR_insert_insn_after (ctx, curr_func_item, prev_call_insn, new_insn);
    next_insn = DLIST_NEXT (MIR_insn_t, new_insn);
    create_new_bb_insns (gen_ctx, prev_call_insn, next_insn, call_insn);
    new_insn = MIR_new_insn (ctx, MIR_ADD, _MIR_new_var_op (ctx, SP_HARD_REG),
                             _MIR_new_var_op (ctx, SP_HARD_REG), MIR_new_int_op (ctx, mem_size));
    MIR_insert_insn_after (ctx, curr_func_item, call_insn, new_insn);
    next_insn = DLIST_NEXT (MIR_insn_t, new_insn);
    create_new_bb_insns (gen_ctx, call_insn, next_insn, call_insn);
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

struct insn_pattern_info {
  int start, num;
};

typedef struct insn_pattern_info insn_pattern_info_t;
DEF_VARR (insn_pattern_info_t);

struct label_ref {
  int abs_addr_p, short_p;
  size_t label_val_disp;
  union {
    MIR_label_t label;
    void *jump_addr; /* absolute addr for BBV */
  } u;
};

typedef struct label_ref label_ref_t;
DEF_VARR (label_ref_t);

struct target_ctx {
  unsigned char alloca_p, block_arg_func_p, leaf_p, short_bb_branch_p;
  size_t small_aggregate_save_area;
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
#define short_bb_branch_p gen_ctx->target_ctx->short_bb_branch_p
#define small_aggregate_save_area gen_ctx->target_ctx->small_aggregate_save_area
#define temp_jump gen_ctx->target_ctx->temp_jump
#define temp_jump_replacement gen_ctx->target_ctx->temp_jump_replacement
#define pattern_indexes gen_ctx->target_ctx->pattern_indexes
#define insn_pattern_info gen_ctx->target_ctx->insn_pattern_info
#define result_code gen_ctx->target_ctx->result_code
#define label_refs gen_ctx->target_ctx->label_refs
#define abs_address_locs gen_ctx->target_ctx->abs_address_locs
#define relocs gen_ctx->target_ctx->relocs

static MIR_disp_t target_get_stack_slot_offset (gen_ctx_t gen_ctx, MIR_type_t type MIR_UNUSED,
                                                MIR_reg_t slot) {
  /* slot is 0, 1, ... */
  size_t offset = curr_func_item->u.func->vararg_p || block_arg_func_p ? 32 : 16;

  return ((MIR_disp_t) slot * 8 + offset);
}

static int target_valid_mem_offset_p (gen_ctx_t gen_ctx MIR_UNUSED, MIR_type_t type,
                                      MIR_disp_t offset) {
  int scale;
  switch (type) {
  case MIR_T_I8:
  case MIR_T_U8: scale = 1; break;
  case MIR_T_I16:
  case MIR_T_U16: scale = 2; break;
  case MIR_T_I32:
  case MIR_T_U32:
#if MIR_PTR32
  case MIR_T_P:
#endif
  case MIR_T_F: scale = 4; break;
  case MIR_T_LD: scale = 16; break;
  default: scale = 8; break;
  }
  return offset >= 0 && offset % scale == 0 && offset / scale < (1 << 12);
}

static void target_machinize (gen_ctx_t gen_ctx) {
  MIR_context_t ctx = gen_ctx->ctx;
  MIR_func_t func;
  MIR_type_t type, mem_type, res_type;
  MIR_insn_code_t code, new_insn_code;
  MIR_insn_t insn, next_insn, new_insn, anchor;
  MIR_var_t var;
  MIR_reg_t ret_reg, arg_reg;
  MIR_op_t ret_reg_op, arg_reg_op, mem_op, temp_op;
  size_t i, int_arg_num, fp_arg_num, mem_size, qwords;

  assert (curr_func_item->item_type == MIR_func_item);
  func = curr_func_item->u.func;
  block_arg_func_p = FALSE;
  anchor = DLIST_HEAD (MIR_insn_t, func->insns);
  small_aggregate_save_area = 0;
  for (i = int_arg_num = fp_arg_num = mem_size = 0; i < func->nargs; i++) {
    /* Argument extensions is already done in simplify */
    /* Prologue: generate arg_var = hard_reg|stack mem|stack addr ... */
    var = VARR_GET (MIR_var_t, func->vars, i);
    type = var.type;
    if (type == MIR_T_RBLK && i == 0) { /* hidden arg */
      arg_reg_op = _MIR_new_var_op (ctx, R8_HARD_REG);
      gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, i + MAX_HARD_REG + 1), arg_reg_op);
      continue;
    } else if (MIR_blk_type_p (type) && (qwords = (var.size + 7) / 8) <= 2) {
      if (int_arg_num + qwords <= 8) {
        small_aggregate_save_area += qwords * 8;
        new_insn = MIR_new_insn (ctx, MIR_SUB, _MIR_new_var_op (ctx, i + MAX_HARD_REG + 1),
                                 _MIR_new_var_op (ctx, FP_HARD_REG),
                                 MIR_new_int_op (ctx, small_aggregate_save_area));
        gen_add_insn_before (gen_ctx, anchor, new_insn);
        if (qwords == 0) continue;
        gen_mov (gen_ctx, anchor, MIR_MOV,
                 _MIR_new_var_mem_op (ctx, MIR_T_I64, 0, i + MAX_HARD_REG + 1, MIR_NON_VAR, 1),
                 _MIR_new_var_op (ctx, int_arg_num));
        if (qwords == 2)
          gen_mov (gen_ctx, anchor, MIR_MOV,
                   _MIR_new_var_mem_op (ctx, MIR_T_I64, 8, i + MAX_HARD_REG + 1, MIR_NON_VAR, 1),
                   _MIR_new_var_op (ctx, int_arg_num + 1));
      } else { /* pass on stack w/o address: */
        if (!block_arg_func_p) {
          block_arg_func_p = TRUE;
          gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, R8_HARD_REG),
                   _MIR_new_var_mem_op (ctx, MIR_T_I64, 16, FP_HARD_REG, MIR_NON_VAR, 1));
        }
        gen_add_insn_before (gen_ctx, anchor,
                             MIR_new_insn (ctx, MIR_ADD,
                                           _MIR_new_var_op (ctx, i + MAX_HARD_REG + 1),
                                           _MIR_new_var_op (ctx, R8_HARD_REG),
                                           MIR_new_int_op (ctx, mem_size)));
        mem_size += qwords * 8;
      }
      int_arg_num += qwords;
      continue;
    }
    arg_reg = get_arg_reg (type, &int_arg_num, &fp_arg_num, &new_insn_code);
    if (arg_reg != MIR_NON_VAR) {
      arg_reg_op = _MIR_new_var_op (ctx, arg_reg);
      gen_mov (gen_ctx, anchor, new_insn_code, _MIR_new_var_op (ctx, i + MAX_HARD_REG + 1),
               arg_reg_op);
    } else {
      /* arg is on the stack */
      if (!block_arg_func_p) {
        block_arg_func_p = TRUE;
        gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, R8_HARD_REG),
                 _MIR_new_var_mem_op (ctx, MIR_T_I64, 16, FP_HARD_REG, MIR_NON_VAR, 1));
      }
      mem_type = type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD ? type : MIR_T_I64;
      if (type == MIR_T_LD) mem_size = (mem_size + 15) / 16 * 16;
      new_insn_code = (type == MIR_T_F    ? MIR_FMOV
                       : type == MIR_T_D  ? MIR_DMOV
                       : type == MIR_T_LD ? MIR_LDMOV
                                          : MIR_MOV);
      mem_op = new_hard_reg_mem_op (gen_ctx, anchor, mem_type, mem_size, R8_HARD_REG);
      gen_mov (gen_ctx, anchor, new_insn_code, _MIR_new_var_op (ctx, i + MAX_HARD_REG + 1), mem_op);
      mem_size += type == MIR_T_LD ? 16 : 8;
    }
  }
  alloca_p = FALSE;
  leaf_p = TRUE;
  for (insn = DLIST_HEAD (MIR_insn_t, func->insns); insn != NULL; insn = next_insn) {
    MIR_item_t proto_item, func_import_item;
    int nargs;

    next_insn = DLIST_NEXT (MIR_insn_t, insn);
    code = insn->code;
    if (code == MIR_LDBEQ || code == MIR_LDBNE || code == MIR_LDBLT || code == MIR_LDBGE
        || code == MIR_LDBGT || code == MIR_LDBLE) {
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
          new_insn = MIR_new_insn (ctx, MIR_MOV, reg_op3,
                                   MIR_new_int_op (ctx, (int64_t) op3.u.var_mem.type));
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
#if !defined(__APPLE__)
      MIR_op_t treg_op = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
#endif
      MIR_op_t prev_sp_op = _MIR_new_var_op (ctx, gen_new_temp_reg (gen_ctx, MIR_T_I64, func));
      MIR_op_t va_op = insn->ops[0];
      MIR_reg_t va_reg;
      int gp_offset, fp_offset;

      assert (func->vararg_p && va_op.mode == MIR_OP_VAR);
      gp_offset = (int_arg_num >= 8 ? 0 : 8 * int_arg_num - 64);
      fp_offset = (fp_arg_num >= 8 ? 0 : 16 * fp_arg_num - 128);
      va_reg = va_op.u.var;
      /* Insns can be not simplified as soon as they match a machine insn.  */
#if !defined(__APPLE__)
      /* mem32[va_reg].__gr_offset = gp_offset; mem32[va_reg].__vr_offset = fp_offset */
      gen_mov (gen_ctx, insn, MIR_MOV, treg_op, MIR_new_int_op (ctx, gp_offset));
      gen_mov (gen_ctx, insn, MIR_MOV,
               _MIR_new_var_mem_op (ctx, MIR_T_U32, 24, va_reg, MIR_NON_VAR, 1), treg_op);
      gen_mov (gen_ctx, insn, MIR_MOV, treg_op, MIR_new_int_op (ctx, fp_offset));
      gen_mov (gen_ctx, insn, MIR_MOV,
               _MIR_new_var_mem_op (ctx, MIR_T_U32, 28, va_reg, MIR_NON_VAR, 1), treg_op);
#endif
      /* __stack: prev_sp = mem64[fp + 16] */
      gen_mov (gen_ctx, insn, MIR_MOV, prev_sp_op,
               _MIR_new_var_mem_op (ctx, MIR_T_I64, 16, FP_HARD_REG, MIR_NON_VAR, 1));
#if defined(__APPLE__)
      gen_mov (gen_ctx, insn, MIR_MOV,
               _MIR_new_var_mem_op (ctx, MIR_T_I64, 0, va_reg, MIR_NON_VAR, 1), prev_sp_op);
#else
      /* mem64[va_reg].__stack = prev_sp + mem_size */
      new_insn = MIR_new_insn (ctx, MIR_ADD, treg_op, prev_sp_op, MIR_new_int_op (ctx, mem_size));
      gen_add_insn_before (gen_ctx, insn, new_insn);
      gen_mov (gen_ctx, insn, MIR_MOV,
               _MIR_new_var_mem_op (ctx, MIR_T_I64, 0, va_reg, MIR_NON_VAR, 1), treg_op);
      /* __gr_top: mem64[va_reg].__gr_top = prev_sp */
      gen_mov (gen_ctx, insn, MIR_MOV,
               _MIR_new_var_mem_op (ctx, MIR_T_I64, 8, va_reg, MIR_NON_VAR, 1), prev_sp_op);
      /* __vr_top: treg = prev_sp - int_reg_save_area; mem64[va_reg].__vr_top = treg */
      new_insn = MIR_new_insn (ctx, MIR_SUB, treg_op, prev_sp_op,
                               MIR_new_int_op (ctx, int_reg_save_area_size));
      gen_add_insn_before (gen_ctx, insn, new_insn);
      gen_mov (gen_ctx, insn, MIR_MOV,
               _MIR_new_var_mem_op (ctx, MIR_T_I64, 16, va_reg, MIR_NON_VAR, 1), treg_op);
#endif
      gen_delete_insn (gen_ctx, insn);
    } else if (code == MIR_VA_END) { /* do nothing */
      gen_delete_insn (gen_ctx, insn);
    } else if (MIR_call_code_p (code)) {
      machinize_call (gen_ctx, insn);
      leaf_p = FALSE;
    } else if (code == MIR_ALLOCA) {
      alloca_p = TRUE;
    } else if (code == MIR_FBLT) { /* don't use blt/ble for correct nan processing: */
      SWAP (insn->ops[1], insn->ops[2], temp_op);
      insn->code = MIR_FBGT;
    } else if (code == MIR_FBLE) {
      SWAP (insn->ops[1], insn->ops[2], temp_op);
      insn->code = MIR_FBGE;
    } else if (code == MIR_DBLT) {
      SWAP (insn->ops[1], insn->ops[2], temp_op);
      insn->code = MIR_DBGT;
    } else if (code == MIR_DBLE) {
      SWAP (insn->ops[1], insn->ops[2], temp_op);
      insn->code = MIR_DBGE;
    } else if (code == MIR_RET) {
      /* In simplify we already transformed code for one return insn
         and added extension insn (if any).  */
      uint32_t n_iregs = 0, n_vregs = 0;

      assert (func->nres == MIR_insn_nops (ctx, insn));
      for (i = 0; i < func->nres; i++) {
        assert (insn->ops[i].mode == MIR_OP_VAR);
        res_type = func->res_types[i];
        if ((res_type == MIR_T_F || res_type == MIR_T_D || res_type == MIR_T_LD) && n_vregs < 8) {
          new_insn_code = res_type == MIR_T_F   ? MIR_FMOV
                          : res_type == MIR_T_D ? MIR_DMOV
                                                : MIR_LDMOV;
          ret_reg = V0_HARD_REG + n_vregs++;
        } else if (n_iregs < 8) {
          new_insn_code = MIR_MOV;
          ret_reg = R0_HARD_REG + n_iregs++;
        } else {
          (*MIR_get_error_func (ctx)) (MIR_ret_error,
                                       "aarch64 can not handle this combination of return values");
        }
        ret_reg_op = _MIR_new_var_op (ctx, ret_reg);
        gen_mov (gen_ctx, insn, new_insn_code, ret_reg_op, insn->ops[i]);
        insn->ops[i] = ret_reg_op;
      }
    }
  }
}

#if !defined(__APPLE__)
static void isave (gen_ctx_t gen_ctx, MIR_insn_t anchor, int disp, MIR_reg_t base,
                   MIR_reg_t hard_reg) {
  gen_mov (gen_ctx, anchor, MIR_MOV, new_hard_reg_mem_op (gen_ctx, anchor, MIR_T_I64, disp, base),
           _MIR_new_var_op (gen_ctx->ctx, hard_reg));
}

static void fsave (gen_ctx_t gen_ctx, MIR_insn_t anchor, int disp, MIR_reg_t base,
                   MIR_reg_t hard_reg) {
  gen_mov (gen_ctx, anchor, MIR_LDMOV, new_hard_reg_mem_op (gen_ctx, anchor, MIR_T_LD, disp, base),
           _MIR_new_var_op (gen_ctx->ctx, hard_reg));
}
#endif

static void target_make_prolog_epilog (gen_ctx_t gen_ctx, bitmap_t used_hard_regs,
                                       size_t stack_slots_num) {
  MIR_context_t ctx = gen_ctx->ctx;
  MIR_func_t func;
  MIR_insn_t anchor, new_insn;
  MIR_op_t sp_reg_op, fp_reg_op, treg_op, treg_op2;
  int save_prev_stack_p;
  size_t i, offset, frame_size, frame_size_after_saved_regs, saved_iregs_num, saved_fregs_num;

  assert (curr_func_item->item_type == MIR_func_item);
  func = curr_func_item->u.func;
  for (i = saved_iregs_num = saved_fregs_num = 0; i <= MAX_HARD_REG; i++)
    if (!target_call_used_hard_reg_p (i, MIR_T_UNDEF) && bitmap_bit_p (used_hard_regs, i)) {
      if (i < V0_HARD_REG)
        saved_iregs_num++;
      else
        saved_fregs_num++;
    }
  if (leaf_p && !alloca_p && saved_iregs_num == 0 && saved_fregs_num == 0 && !func->vararg_p
      && stack_slots_num == 0 && !block_arg_func_p && small_aggregate_save_area == 0)
    return;
  sp_reg_op = _MIR_new_var_op (ctx, SP_HARD_REG);
  fp_reg_op = _MIR_new_var_op (ctx, FP_HARD_REG);
  /* Prologue: */
  anchor = DLIST_HEAD (MIR_insn_t, func->insns);
#if defined(__APPLE__)
  frame_size = 0;
#else
  frame_size = func->vararg_p ? reg_save_area_size : 0;
#endif
  for (i = 0; i <= MAX_HARD_REG; i++)
    if (!target_call_used_hard_reg_p (i, MIR_T_UNDEF) && bitmap_bit_p (used_hard_regs, i)) {
      if (i < V0_HARD_REG) {
        frame_size += 8;
      } else {
        if (frame_size % 16 != 0) frame_size = (frame_size + 15) / 16 * 16;
        frame_size += 16;
      }
    }
  if (frame_size % 16 != 0) frame_size = (frame_size + 15) / 16 * 16;
  frame_size_after_saved_regs = frame_size;
  frame_size += stack_slots_num * 8;
  if (frame_size % 16 != 0) frame_size = (frame_size + 15) / 16 * 16;
  save_prev_stack_p = func->vararg_p || block_arg_func_p;
  treg_op = _MIR_new_var_op (ctx, R9_HARD_REG);
  if (save_prev_stack_p) { /* prev stack pointer */
    gen_mov (gen_ctx, anchor, MIR_MOV, treg_op, sp_reg_op);
    frame_size += 16;
  }
  frame_size += 16; /* lr/fp */
  treg_op2 = _MIR_new_var_op (ctx, R10_HARD_REG);
  if (frame_size < (1 << 12)) {
    new_insn = MIR_new_insn (ctx, MIR_SUB, sp_reg_op, sp_reg_op, MIR_new_int_op (ctx, frame_size));
  } else {
    new_insn = MIR_new_insn (ctx, MIR_MOV, treg_op2, MIR_new_int_op (ctx, frame_size));
    gen_add_insn_before (gen_ctx, anchor, new_insn); /* t = frame_size */
    new_insn = MIR_new_insn (ctx, MIR_SUB, sp_reg_op, sp_reg_op, treg_op2);
  }
  gen_add_insn_before (gen_ctx, anchor, new_insn); /* sp = sp - (frame_size|t) */
  if (save_prev_stack_p)
    gen_mov (gen_ctx, anchor, MIR_MOV,
             _MIR_new_var_mem_op (ctx, MIR_T_I64, 16, SP_HARD_REG, MIR_NON_VAR, 1),
             treg_op); /* mem[sp + 16] = treg */
  if (!func->jret_p)
    gen_mov (gen_ctx, anchor, MIR_MOV,
             _MIR_new_var_mem_op (ctx, MIR_T_I64, 8, SP_HARD_REG, MIR_NON_VAR, 1),
             _MIR_new_var_op (ctx, LINK_HARD_REG)); /* mem[sp + 8] = lr */
  gen_mov (gen_ctx, anchor, MIR_MOV,
           _MIR_new_var_mem_op (ctx, MIR_T_I64, 0, SP_HARD_REG, MIR_NON_VAR, 1),
           _MIR_new_var_op (ctx, FP_HARD_REG));             /* mem[sp] = fp */
  gen_mov (gen_ctx, anchor, MIR_MOV, fp_reg_op, sp_reg_op); /* fp = sp */
#if !defined(__APPLE__)
  if (func->vararg_p) {  // ??? saving only regs corresponding to ...
    MIR_reg_t base = SP_HARD_REG;
    int64_t start;

    start = (int64_t) frame_size - reg_save_area_size;
    if ((start + 184) >= (1 << 12)) {
      new_insn = MIR_new_insn (ctx, MIR_MOV, treg_op, MIR_new_int_op (ctx, start));
      gen_add_insn_before (gen_ctx, anchor, new_insn); /* t = frame_size - reg_save_area_size */
      start = 0;
      base = R9_HARD_REG;
    }
    fsave (gen_ctx, anchor, start, base, V0_HARD_REG);
    fsave (gen_ctx, anchor, start + 16, base, V1_HARD_REG);
    fsave (gen_ctx, anchor, start + 32, base, V2_HARD_REG);
    fsave (gen_ctx, anchor, start + 48, base, V3_HARD_REG);
    fsave (gen_ctx, anchor, start + 64, base, V4_HARD_REG);
    fsave (gen_ctx, anchor, start + 80, base, V5_HARD_REG);
    fsave (gen_ctx, anchor, start + 96, base, V6_HARD_REG);
    fsave (gen_ctx, anchor, start + 112, base, V7_HARD_REG);
    isave (gen_ctx, anchor, start + 128, base, R0_HARD_REG);
    isave (gen_ctx, anchor, start + 136, base, R1_HARD_REG);
    isave (gen_ctx, anchor, start + 144, base, R2_HARD_REG);
    isave (gen_ctx, anchor, start + 152, base, R3_HARD_REG);
    isave (gen_ctx, anchor, start + 160, base, R4_HARD_REG);
    isave (gen_ctx, anchor, start + 168, base, R5_HARD_REG);
    isave (gen_ctx, anchor, start + 176, base, R6_HARD_REG);
    isave (gen_ctx, anchor, start + 184, base, R7_HARD_REG);
  }
#endif
  /* Saving callee saved hard registers: */
  offset = frame_size - frame_size_after_saved_regs;
  for (i = 0; i <= MAX_HARD_REG; i++)
    if (!target_call_used_hard_reg_p (i, MIR_T_UNDEF) && bitmap_bit_p (used_hard_regs, i)) {
      if (i < V0_HARD_REG) {
        gen_mov (gen_ctx, anchor, MIR_MOV,
                 new_hard_reg_mem_op (gen_ctx, anchor, MIR_T_I64, offset, FP_HARD_REG),
                 _MIR_new_var_op (ctx, i));
        offset += 8;
      } else {
        if (offset % 16 != 0) offset = (offset + 15) / 16 * 16;
        new_insn = gen_mov (gen_ctx, anchor, MIR_LDMOV,
                            new_hard_reg_mem_op (gen_ctx, anchor, MIR_T_LD, offset, FP_HARD_REG),
                            _MIR_new_var_op (ctx, i));
#if defined(__APPLE__)
        /* MIR API can change insn code - change it back as we need to generate code to save all
         * vreg. */
        if (new_insn->code == MIR_DMOV) new_insn->code = MIR_LDMOV;
#endif
        offset += 16;
      }
    }
  if (small_aggregate_save_area != 0) {  // ??? duplication with vararg saved regs
    if (small_aggregate_save_area % 16 != 0)
      small_aggregate_save_area = (small_aggregate_save_area + 15) / 16 * 16;
    new_insn = MIR_new_insn (ctx, MIR_SUB, sp_reg_op, sp_reg_op,
                             MIR_new_int_op (ctx, small_aggregate_save_area));
    gen_add_insn_before (gen_ctx, anchor, new_insn); /* sp -= <small aggr save area size> */
  }
  /* Epilogue: */
  for (anchor = DLIST_TAIL (MIR_insn_t, func->insns); anchor != NULL;
       anchor = DLIST_PREV (MIR_insn_t, anchor))
    if (anchor->code == MIR_RET || anchor->code == MIR_JRET) break;
  if (anchor == NULL) return;
  /* Restoring hard registers: */
  offset = frame_size - frame_size_after_saved_regs;
  for (i = 0; i <= MAX_HARD_REG; i++)
    if (!target_call_used_hard_reg_p (i, MIR_T_UNDEF) && bitmap_bit_p (used_hard_regs, i)) {
      if (i < V0_HARD_REG) {
        gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, i),
                 new_hard_reg_mem_op (gen_ctx, anchor, MIR_T_I64, offset, FP_HARD_REG));
        offset += 8;
      } else {
        if (offset % 16 != 0) offset = (offset + 15) / 16 * 16;
        new_insn = gen_mov (gen_ctx, anchor, MIR_LDMOV, _MIR_new_var_op (ctx, i),
                            new_hard_reg_mem_op (gen_ctx, anchor, MIR_T_LD, offset, FP_HARD_REG));
#if defined(__APPLE__)
        if (new_insn->code == MIR_DMOV) new_insn->code = MIR_LDMOV;
#endif
        offset += 16;
      }
    }
  /* Restore lr, sp, fp */
  if (!func->jret_p)
    gen_mov (gen_ctx, anchor, MIR_MOV, _MIR_new_var_op (ctx, LINK_HARD_REG),
             _MIR_new_var_mem_op (ctx, MIR_T_I64, 8, FP_HARD_REG, MIR_NON_VAR, 1));
  gen_mov (gen_ctx, anchor, MIR_MOV, treg_op2, fp_reg_op); /* r10 = fp */
  gen_mov (gen_ctx, anchor, MIR_MOV, fp_reg_op,
           _MIR_new_var_mem_op (ctx, MIR_T_I64, 0, FP_HARD_REG, MIR_NON_VAR, 1));
  if (frame_size < (1 << 12)) {
    new_insn = MIR_new_insn (ctx, MIR_ADD, sp_reg_op, treg_op2, MIR_new_int_op (ctx, frame_size));
  } else {
    new_insn = MIR_new_insn (ctx, MIR_MOV, treg_op, MIR_new_int_op (ctx, frame_size));
    gen_add_insn_before (gen_ctx, anchor, new_insn); /* t(r9) = frame_size */
    new_insn = MIR_new_insn (ctx, MIR_ADD, sp_reg_op, treg_op2, treg_op);
  }
  gen_add_insn_before (gen_ctx, anchor, new_insn); /* sp = r10 + (frame_size|t) */
}

struct pattern {
  MIR_insn_code_t code;
  /* Pattern elements:
     blank - ignore
     X - match everything
     $ - finish successfully matching
     r - register
     h[0-63] - hard register with given number

        memory with indexed reg offset:
     m[0-3] - int (signed or unsigned) type memory of size 8,16,32,64-bits
     ms[0-3] - signed int type memory of size 8,16,32,64-bits
     mu[0-3] - unsigned int type memory of size 8,16,32,64-bits
       option(field[13..15]) == 011 -- shifted reg (Rm=R31 means SP)
       option == 010 (UXTW), 110 (SXTW), 111 (SXTX) -- extended reg (RM=R31 means ZR)
       we use option == 111 only for non-index mem and 011 for indexed memory

       memory with immediate offset:
     M[0-3] - int (signed or unsigned) type memory of size 8,16,32,64-bits
     Ms[0-3] - signed int type memory of size 8,16,32,64-bits
     Mu[0-3] - unsigned int type memory of size 8,16,32,64-bits
       zero extended scaled 12-bit offset (field[10..21])

     N[0-2] - 2nd immediate (or reference) operand can be created by movn and n movk insns
     Z[0-2] - 2nd immediate (or reference) operand can be created by movz and n movk insns
     Z3 - any 2nd 64-bit immediate (or reference) operand

     Zf - floating point 0.0
     Zd - double floating point 0.0

       memory with indexed reg offset:
     mf - memory of float
     md - memory of double
     mld - memory of long double

       memory with immediate offset:
     Mf - memory of float
     Md - memory of double
     Mld - memory of long double
     I -- immediate as 3th op for arithmetic insn (12-bit unsigned with possible 12-bit LSL)
     Iu -- immediate for arithmetic insn roundup to 16
     SR -- any immediate for right 64-bit shift (0-63)
     Sr -- any immediate for right 32-bit shift (0-31)
     SL -- any immediate for left 64-bit shift (0-63)
     Sl -- any immediate for left 32-bit shift (0-31)
     L - reference or label as the 1st or 2nd op which can be present by signed 26-bit pc offset
     (in 4 insn bytes) l - label as the 1st op which can be present by signed 19-bit pc offset (in
     4 insn bytes)

     Remember we have no float or (long) double immediate at this stage. They are represented
     by a reference to data item.  */

  const char *pattern;
  /* Replacement elements:
     blank - ignore
     ; - insn separation
     hex:hex - opcode and its mask (the mask should include opcode, the mask defines bits
                                    which can not be changed by other fields)
     rd[0-2] - put n-th operand register into rd field [0..4]
     rn[0-2] - put n-th operand register into rn field [5..9]
     rm[0-2] - put n-th operand register into rm field [16..20]
     ra[0-2] - put n-th operand register into ra field [10..14]
     h(d,n,m)<one or two hex digits> - hardware register with given number in rd,rn,rm field
     m = 1st or 2nd operand is (8-,16-,32-,64-bit) mem with base, scaled index
     M = 1st or 2nd operand is (8-,16-,32-,64-bit) mem with base, scaled imm12 disp [10..21]
     S - immr[16..21]  for right shift SR/Sr
     SL, Sl - immr[16..21] and imms[10..15] for left shift SL/Sl

     Z[0-3] -- n-th 16-bit immediate[5..20] from Z[0-3] and its shift [21..22]
     N[0-3] -- n-th 16-bit immediate[5..20] from N[0-3] and its shift [21..22]
     I -- arithmetic op 12-bit immediate [10..21] and its shift [22..23]
     Iu -- arithmetic op immediate [10..21] got from roundup value to 16 and its shift [22..23]
     L -- operand-label as 26-bit offset
     l -- operand-label as 19-bit offset
     T -- pc-relative address [5..23]
     i<one or two hex digits> -- shift value in [10..15]
     I<one or two hex digits> -- shift value in [16..21]
  */
  const char *replacement;
};

#define SUB_UBO MIR_INSN_BOUND
#define SUB_UBNO (SUB_UBO + 1)
#define MUL_BO (SUB_UBNO + 1)
#define MUL_BNO (MUL_BO + 1)
#define ARM_INSN_BOUND (MUL_BNO + 1)

static const struct pattern patterns[] = {
  {MIR_MOV, "r h31", "91000000:fffffc00 rd0 hn1f"}, /* mov Rd,SP */
  {MIR_MOV, "h31 r", "91000000:fffffc00 hd1f rn1"}, /* mov SP,Rn */
  {MIR_MOV, "r r", "aa0003e0:ffe0ffe0 rd0 rm1"},    /* mov Rd,Rm */

  // ??? patterns to extend 32-bit index register
  {MIR_MOV, "r m3", "f8600800:ffe00c00 rd0 m"}, /* ldr Rd,[Rn,Rm{,#3}] */
  {MIR_MOV, "m3 r", "f8200800:ffe00c00 rd1 m"}, /* str Rd,[Rn,Rm{,#3}] */
  {MIR_MOV, "r M3", "f9400000:ffc00000 rd0 M"}, /* ldr Rd,[Rn,{,#imm12}] */
  {MIR_MOV, "M3 r", "f9000000:ffc00000 rd1 M"}, /* str Rd,[Rn,Rm{,#imm12}] */

  {MIR_MOV, "r mu2", "b8600800:ffe00c00 rd0 m"}, /* ldr Wd,[Rn,Rm{,#2}] */
  {MIR_MOV, "m2 r", "b8200800:ffe00c00 rd1 m"},  /* str Wd,[Rn,Rm{,#2}] */
  {MIR_MOV, "r Mu2", "b9400000:ffc00000 rd0 M"}, /* ldr Wd,[Rn{,#imm12}] */
  {MIR_MOV, "M2 r", "b9000000:ffc00000 rd1 M"},  /* str Wd,[Rn,Rm{,#imm12}] */

  {MIR_MOV, "r ms2", "b8a00800:ffe00c00 rd0 m"}, /* ldrsw Rd,[Rn,Rm{,#2}] */
  {MIR_MOV, "r Ms2", "b9800000:ffc00000 rd0 M"}, /* ldrsw Rd,[Rn{,#imm12}] */

  {MIR_MOV, "r mu1", "78600800:ffe00c00 rd0 m"}, /* ldrh Wd,[Rn,Rm{,#1}] */
  {MIR_MOV, "m1 r", "78200800:ffe00c00 rd1 m"},  /* strh Wd,[Rn,Rm{,#1}] */
  {MIR_MOV, "r Mu1", "79400000:ffc00000 rd0 M"}, /* ldrh Wd,[Rn{,#imm12}] */
  {MIR_MOV, "M1 r", "79000000:ffc00000 rd1 M"},  /* strh Wd,[Rn,Rm{,#imm12}] */

  {MIR_MOV, "r ms1", "78a00800:ffe00c00 rd0 m"}, /* ldrsh Wd,[Rn,Rm{,#2}] */
  {MIR_MOV, "r Ms1", "79800000:ffc00000 rd0 M"}, /* ldrsh Wd,[Rn{,#imm12}] */

  {MIR_MOV, "r mu0", "38600800:ffe00c00 rd0 m"}, /* ldrb Wd,[Rn,Rm{,#1}] */
  {MIR_MOV, "m0 r", "38200800:ffe00c00 rd1 m"},  /* strb Wd,[Rn,Rm{,#1}] */
  {MIR_MOV, "r Mu0", "39400000:ffc00000 rd0 M"}, /* ldrb Wd,[Rn{,#imm12}] */
  {MIR_MOV, "M0 r", "39000000:ffc00000 rd1 M"},  /* strb Wd,[Rn,Rm{,#imm12}] */

  {MIR_MOV, "r ms0", "38a00800:ffa00c00 rd0 m"}, /* ldrsb Rd,[Rn,Rm{,#1}] */
  {MIR_MOV, "r Ms0", "39800000:ffc00000 rd0 M"}, /* ldrsb Rd,[Rn{,#imm12}] */

  {MIR_MOV, "r Z0", "d2800000:ff800000 rd0 Z0"}, /* movz Rd, imm */
  {MIR_MOV, "r N0", "92800000:ff800000 rd0 N0"}, /* movn Rd, imm */
  /* movn Rd, imm0, sh0; movk Rd, imm1, sh1: */
  {MIR_MOV, "r Z1", "d2800000:ff800000 rd0 Z0; f2800000:ff800000 rd0 Z1"},
  /* movn imm0, sh0; movk Rd, imm1, sh1:  */
  {MIR_MOV, "r N1", "92800000:ff800000 rd0 N0; f2800000:ff800000 rd0 N1"},
  /* movz Rd, imm0, sh0; movk Rd, imm1, sh1; movk Rd, imm3, sh3: */
  {MIR_MOV, "r Z2", "d2800000:ff800000 rd0 Z0; f2800000:ff800000 rd0 Z1; f2800000:ff800000 rd0 Z2"},
  /* movn Rd, imm0, sh0; movk Rd, imm1, sh1; movk Rd, imm3, sh3: */
  {MIR_MOV, "r N2", "92800000:ff800000 rd0 N0; f2800000:ff800000 rd0 N1; f2800000:ff800000 rd0 N2"},
  /* movz Rd, imm0, sh0; movk Rd, imm1, sh1; movk Rd, imm2, sh2; movk Rd, imm3, sh3: */
  {MIR_MOV, "r Z3",
   "d2800000:ff800000 rd0 Z0; f2800000:ff800000 rd0 Z1; f2800000:ff800000 rd0 Z2;"
   "f2800000:ff800000 rd0 Z3"},

  {MIR_FMOV, "r r", "1e204000:fffffc00 vd0 vn1"}, /* fmov Sd,Sn */
  {MIR_FMOV, "r mf", "bc600800:ff600c00 vd0 m"},  /* ldr Sd,[Rn,Rm{,#2}] */
  {MIR_FMOV, "mf r", "bc200800:ff600c00 vd1 m"},  /* str Sd,[Rn,Rm{,#2}] */
  {MIR_FMOV, "r Mf", "bd400000:ffc00000 vd0 M"},  /* ldr Sd,[Rn,{,#imm12}] */
  {MIR_FMOV, "Mf r", "bd000000:ffc00000 vd1 M"},  /* str Sd,[Rn,Rm{,#imm12}] */

  {MIR_DMOV, "r r", "1e604000:fffffc00 vd0 vn1"}, /* fmov Dd,Dn */
  {MIR_DMOV, "r md", "fc600800:ff600c00 vd0 m"},  /* ldr Dd,[Rn,Rm{,#3}] */
  {MIR_DMOV, "md r", "fc200800:ff600c00 vd1 m"},  /* str Dd,[Rn,Rm{,#3}] */
  {MIR_DMOV, "r Md", "fd400000:ffc00000 vd0 M"},  /* ldr Dd,[Rn,{,#imm12}] */
  {MIR_DMOV, "Md r", "fd000000:ffc00000 vd1 M"},  /* str Dd,[Rn,Rm{,#imm12}] */

  {MIR_LDMOV, "r r", "4ea01c00:ffe0fc00 vd0 vm1 vn1"}, /* orr Qd.16b,Qm.16b,Qn.16b */
  {MIR_LDMOV, "r mld", "3ce00800:ffe00c00 vd0 m"},     /* ldr Qd,[Rn,Rm{,#4}] */
  {MIR_LDMOV, "mld r", "3ca00800:ffe00c00 vd1 m"},     /* str Qd,[Rn,Rm{,#4}] */
  {MIR_LDMOV, "r Mld", "3dc00000:ffc00000 vd0 M"},     /* ldr Qd,[Rn,{,#imm12}] */
  {MIR_LDMOV, "Mld r", "3d800000:ffc00000 vd1 M"},     /* str Qd,[Rn,Rm{,#imm12}] */

  {MIR_EXT8, "r r", "93401c00:fffffc00 rd0 rn1"},  /* sxtb rd, wn */
  {MIR_EXT16, "r r", "93403c00:fffffc00 rd0 rn1"}, /* sxth rd, wn */
  {MIR_EXT32, "r r", "93407c00:fffffc00 rd0 rn1"}, /* sxtw rd, wn */

  {MIR_UEXT8, "r r", "53001c00:fffffc00 rd0 rn1"},  /* uxtb wd, wn */
  {MIR_UEXT16, "r r", "53003c00:fffffc00 rd0 rn1"}, /* uxth wd, wn */
  {MIR_UEXT32, "r r", "2a0003e0:7fe0ffe0 rd0 rm1"}, /* mov wd, wm */

#define IOP(icode, rop, iop, rops, iops)                                         \
  {icode, "r r r", rop ":ffe0fc00 rd0 rn1 rm2"},       /* extended op Rd,Rn,Rm*/ \
    {icode, "r r I", iop ":ff000000 rd0 rn1 I"},       /* op Rd,Rn,I,shift */    \
    {icode##S, "r r r", rops ":ff200000 rd0 rn1 rm2"}, /* op Wd,Wn,Wm*/          \
    {icode##S, "r r I", iops ":ff000000 rd0 rn1 I"},   /* op Wd,Wn,I,shift */

  IOP (MIR_ADD, "8b206000", "91000000", "0b000000", "11000000")
    IOP (MIR_ADDO, "ab206000", "b1000000", "2b000000", "31000000")

      {MIR_FADD, "r r r", "1e202800:ffe0fc00 vd0 vn1 vm2"}, /* fadd Sd,Sn,Sm*/
  {MIR_DADD, "r r r", "1e602800:ffe0fc00 vd0 vn1 vm2"},     /* fadd Dd,Dn,Dm*/
  // ldadd is implemented through builtin

  IOP (MIR_SUB, "cb206000", "d1000000", "4b000000", "51000000")
    IOP (MIR_SUBO, "eb206000", "f1000000", "6b000000", "71000000")

      {MIR_FSUB, "r r r", "1e203800:ffe0fc00 vd0 vn1 vm2"}, /* fsub Sd,Sn,Sm*/
  {MIR_DSUB, "r r r", "1e603800:ffe0fc00 vd0 vn1 vm2"},     /* fsub Dd,Dn,Dm*/
  // ldsub is implemented through builtin

  {MIR_MUL, "r r r", "9b007c00:ffe0fc00 rd0 rn1 rm2"},  /* mul Rd,Rn,Rm*/
  {MIR_MULS, "r r r", "1b007c00:ffe0fc00 rd0 rn1 rm2"}, /* mul Wd,Wn,Wm*/
  {MIR_FMUL, "r r r", "1e200800:ffe0fc00 vd0 vn1 vm2"}, /* fmul Sd,Sn,Sm*/
  {MIR_DMUL, "r r r", "1e600800:ffe0fc00 vd0 vn1 vm2"}, /* fmul Dd,Dn,Dm*/
  // ldmul is implemented through builtin

  {MIR_DIV, "r r r", "9ac00c00:ffe0fc00 rd0 rn1 rm2"},   /* sdiv Rd,Rn,Rm*/
  {MIR_DIVS, "r r r", "1ac00c00:ffe0fc00 rd0 rn1 rm2"},  /* sdiv Wd,Wn,Wm*/
  {MIR_UDIV, "r r r", "9ac00800:ffe0fc00 rd0 rn1 rm2"},  /* udiv Rd,Rn,Rm*/
  {MIR_UDIVS, "r r r", "1ac00800:ffe0fc00 rd0 rn1 rm2"}, /* udiv Wd,Wn,Wm*/
  {MIR_FDIV, "r r r", "1e201800:ffe0fc00 vd0 vn1 vm2"},  /* fdiv Sd,Sn,Sm*/
  {MIR_DDIV, "r r r", "1e601800:ffe0fc00 vd0 vn1 vm2"},  /* fmul Dd,Dn,Dm*/
  // lddiv is implemented through builtin

  /* sdiv r8,Rn,Rm;msub Rd,r8,Rm,Rn: */
  {MIR_MOD, "r r r", "9ac00c00:ffe0fc00 hd8 rn1 rm2;9b008000:ffe08000 rd0 hm8 rn2 ra1"},
  /* sdiv r8,Wn,Wm;msub Wd,r8,Wm,Wn: */
  {MIR_MODS, "r r r", "1ac00c00:ffe0fc00 hd8 rn1 rm2;1b008000:ffe08000 rd0 hm8 rn2 ra1"},
  /* udiv r8,Rn,Rm;msub Rd,r8,Rm,Rn: */
  {MIR_UMOD, "r r r", "9ac00800:ffe0fc00 hd8 rn1 rm2;9b008000:ffe08000 rd0 hm8 rn2 ra1"},
  /* udiv r8,Wn,Wm;msub Wd,r8,Wm,Wn: */
  {MIR_UMODS, "r r r", "1ac00800:ffe0fc00 hd8 rn1 rm2;1b008000:ffe08000 rd0 hm8 rn2 ra1"},

#define CMPR "eb00001f:ff20001f rn1 rm2"
#define CMPI "f100001f:ff00001f rn1 I"
#define SCMPR "6b00001f:ff20001f rn1 rm2"
#define SCMPI "7100001f:ff00001f rn1 I"
#define FCMP "1e202010:ffe0fc1f vn1 vm2"
#define DCMP "1e602010:ffe0fc1f vn1 vm2"

#define REQ "9a9f17e0:ffffffe0 rd0"
#define REQS "1a9f17e0:ffffffe0 rd0"
  // ??? add extended reg cmp insns:
  // all ld insn are changed to builtins
  /* cmp Rn,Rm; cset Rd,eq */
  {MIR_EQ, "r r r", CMPR "; " REQ},
  /* cmp Rn,I,shift ; cset Wd,eq */
  {MIR_EQ, "r r I", CMPI "; " REQ},
  /* cmp Wn,Wm;cset Rd,eq */
  {MIR_EQS, "r r r", SCMPR "; " REQS},
  /* cmp Wn,I,shift; cset Wd,eq */
  {MIR_EQS, "r r I", SCMPI "; " REQS},
  /* fcmpe Sn,Sm; cset Rd,mi */
  {MIR_FEQ, "r r r", FCMP "; " REQ},
  /* fcmpe Dn,Dm; cset Rd,mi */
  {MIR_DEQ, "r r r", DCMP "; " REQ},
  /* fcmpe Sn,0.0; cset Rd,mi */
  {MIR_FEQ, "r r Zf", "1e202018:fffffc1f vn1 vm2; " REQ},
  /* fcmpe Dn,0.0; cset Rd,mi */
  {MIR_DEQ, "r r Zd", "1e602018:fffffc1f vn1 vm2; " REQ},

#define RNE "9a9f07e0:ffffffe0 rd0"
#define RNES "1a9f07e0:ffffffe0 rd0"
  /* cmp Rn,Rm; cset Rd,ne */
  {MIR_NE, "r r r", CMPR "; " RNE},
  /* cmp Rn,I,shift ; cset Wd,ne */
  {MIR_NE, "r r I", CMPI "; " RNE},
  /* cmp Wn,Wm;cset Rd,ne */
  {MIR_NES, "r r r", SCMPR "; " RNES},
  /* cmp Wn,I,shift; cset Wd,ne */
  {MIR_NES, "r r I", SCMPI "; " RNES},
  /* fcmpe Sn,Sm; cset Rd,ne */
  {MIR_FNE, "r r r", FCMP "; " RNE},
  /* fcmpe Dn,Dm; cset Rd,ne */
  {MIR_DNE, "r r r", DCMP "; " RNE},
  /* fcmpe Sn,0.0; cset Rd,ne */
  {MIR_FNE, "r r Zf", "1e202018:fffffc1f vn1 vm2; " RNE},
  /* fcmpe Dn,0.0; cset Rd,ne */
  {MIR_DNE, "r r Zd", "1e602018:fffffc1f vn1 vm2; " RNE},

#define RLT "9a9fa7e0:ffffffe0 rd0"
#define RLTS "1a9fa7e0:ffffffe0 rd0"
#define RULT "9a9f27e0:ffffffe0 rd0"
#define RULTS "1a9f27e0:ffffffe0 rd0"
#define FLTC "9a9f57e0:ffffffe0 rd0"
  /* cmp Rn,Rm; cset Rd,lt */
  {MIR_LT, "r r r", CMPR "; " RLT},
  /* cmp Rn,I,shift ; cset Wd,lt */
  {MIR_LT, "r r I", CMPI "; " RLT},
  /* cmp Wn,Wm;cset Rd,lt */
  {MIR_LTS, "r r r", SCMPR "; " RLTS},
  /* cmp Wn,I,shift; cset Wd,lt */
  {MIR_LTS, "r r I", SCMPI "; " RLTS},
  /* cmp Rn,Rm; cset Rd,ult */
  {MIR_ULT, "r r r", CMPR "; " RULT},
  /* cmp Rn,I,shift ; cset Wd,cc */
  {MIR_ULT, "r r I", CMPI "; " RULT},
  /* cmp Wn,Wm;cset Rd,cc */
  {MIR_ULTS, "r r r", SCMPR "; " RULTS},
  /* cmp Wn,I,shift; cset Wd,cc */
  {MIR_ULTS, "r r I", SCMPI "; " RULTS},
  /* fcmpe Sn,Sm; cset Rd,mi */
  {MIR_FLT, "r r r", FCMP "; " FLTC},
  /* fcmpe Dn,Dm; cset Rd,mi */
  {MIR_DLT, "r r r", DCMP "; " FLTC},
  /* fcmpe Sn,0.0; cset Rd,mi */
  {MIR_FLT, "r r Zf", "1e202018:fffffc1f vn1 vm2; " FLTC},
  /* fcmpe Dn,0.0; cset Rd,mi */
  {MIR_DLT, "r r Zd", "1e602018:fffffc1f vn1 vm2; " FLTC},

#define RGE "9a9fb7e0:ffffffe0 rd0"
#define RGES "1a9fb7e0:ffffffe0 rd0"
#define RUGE "9a9f37e0:ffffffe0 rd0"
#define RUGES "1a9f37e0:ffffffe0 rd0"
  /* cmp Rn,Rm; cset Rd,ge */
  {MIR_GE, "r r r", CMPR "; " RGE},
  /* cmp Rn,I,shift ; cset Wd,ge */
  {MIR_GE, "r r I", CMPI "; " RGE},
  /* cmp Wn,Wm;cset Rd,ge */
  {MIR_GES, "r r r", SCMPR "; " RGES},
  /* cmp Wn,I,shift; cset Wd,ge */
  {MIR_GES, "r r I", SCMPI "; " RGES},
  /* cmp Rn,Rm; cset Rd,cs */
  {MIR_UGE, "r r r", CMPR "; " RUGE},
  /* cmp Rn,I,shift ; cset Wd,cs */
  {MIR_UGE, "r r I", CMPI "; " RUGE},
  /* cmp Wn,Wm;cset Rd,cs */
  {MIR_UGES, "r r r", SCMPR "; " RUGES},
  /* cmp Wn,I,shift; cset Wd,cs */
  {MIR_UGES, "r r I", SCMPI "; " RUGES},
  /* fcmpe Sn,Sm; cset Rd,ge */
  {MIR_FGE, "r r r", FCMP "; " RGE},
  /* fcmpe Dn,Dm; cset Rd,ge */
  {MIR_DGE, "r r r", DCMP "; " RGE},
  /* fcmpe Sn,0.0; cset Rd,ge */
  {MIR_FGE, "r r Zf", "1e202018:fffffc1f vn1 vm2; " RGE},
  /* fcmpe Dn,0.0; cset Rd,ge */
  {MIR_DGE, "r r Zd", "1e602018:fffffc1f vn1 vm2; " RGE},

#define RGT "9a9fd7e0:ffffffe0 rd0"
#define RGTS "1a9fd7e0:ffffffe0 rd0"
#define RUGT "9a9f97e0:ffffffe0 rd0"
#define RUGTS "1a9f97e0:ffffffe0 rd0"
  /* cmp Rn,Rm; cset Rd,gt */
  {MIR_GT, "r r r", CMPR "; " RGT},
  /* cmp Rn,I,shift ; cset Wd,gt */
  {MIR_GT, "r r I", CMPI "; " RGT},
  /* cmp Wn,Wm;cset Rd,gt */
  {MIR_GTS, "r r r", SCMPR "; " RGTS},
  /* cmp Wn,I,shift; cset Wd,gt */
  {MIR_GTS, "r r I", SCMPI "; " RGTS},
  /* cmp Rn,Rm; cset Rd,hi */
  {MIR_UGT, "r r r", CMPR "; " RUGT},
  /* cmp Rn,I,shift ; cset Wd,hi */
  {MIR_UGT, "r r I", CMPI "; " RUGT},
  /* cmp Wn,Wm;cset Rd,hi */
  {MIR_UGTS, "r r r", SCMPR "; " RUGTS},
  /* cmp Wn,I,shift; cset Wd,hi */
  {MIR_UGTS, "r r I", SCMPI "; " RUGTS},
  /* fcmpe Sn,Sm; cset Rd,gt */
  {MIR_FGT, "r r r", FCMP "; " RGT},
  /* fcmpe Dn,Dm; cset Rd,gt */
  {MIR_DGT, "r r r", DCMP "; " RGT},
  /* fcmpe Sn,0.0; cset Rd,gt */
  {MIR_FGT, "r r Zf", "1e202018:fffffc1f vn1 vm2; " RGT},
  /* fcmpe Dn,0.0; cset Rd,gt */
  {MIR_DGT, "r r Zd", "1e602018:fffffc1f vn1 vm2; " RGT},

#define RLE "9a9fc7e0:ffffffe0 rd0"
#define RLES "1a9fc7e0:ffffffe0 rd0"
#define RULE "9a9f87e0:ffffffe0 rd0"
#define RULES "1a9f87e0:ffffffe0 rd0"
#define FLEC "9a9f87e0:ffffffe0 rd0"
  /* cmp Rn,Rm; cset Rd,le */
  {MIR_LE, "r r r", CMPR "; " RLE},
  /* cmp Rn,I,shift ; cset Wd,le */
  {MIR_LE, "r r I", CMPI "; " RLE},
  /* cmp Wn,Wm;cset Rd,le */
  {MIR_LES, "r r r", SCMPR "; " RLES},
  /* cmp Wn,I,shift; cset Wd,le */
  {MIR_LES, "r r I", SCMPI "; " RLES},
  /* cmp Rn,Rm; cset Rd,ls */
  {MIR_ULE, "r r r", CMPR "; " RULE},
  /* cmp Rn,I,shift ; cset Wd,ls */
  {MIR_ULE, "r r I", CMPI "; " RULE},
  /* cmp Wn,Wm;cset Rd,ls */
  {MIR_ULES, "r r r", SCMPR "; " RULES},
  /* cmp Wn,I,shift; cset Wd,ls */
  {MIR_ULES, "r r I", SCMPI "; " RULES},
  /* fcmpe Sn,Sm; cset Rd,ls */
  {MIR_FLE, "r r r", FCMP "; " FLEC},
  /* fcmpe Dn,Dm; cset Rd,ls */
  {MIR_DLE, "r r r", DCMP "; " FLEC},
  /* fcmpe Sn,0.0; cset Rd,ls */
  {MIR_FLE, "r r Zf", "1e202018:fffffc1f vn1 vm2; " FLEC},
  /* fcmpe Dn,0.0; cset Rd,ls */
  {MIR_DLE, "r r Zd", "1e602018:fffffc1f vn1 vm2; " FLEC},

  {MIR_JMP, "L", "14000000:fc000000 L"}, /* 26-bit offset jmp */

  {MIR_LADDR, "r l", "10000000:ff000000 rd0 l"}, /* adr r, L ip-relative address */
  {MIR_JMPI, "r", "d61f0000:fffffc00 rn0"},      /* jmp *r */

  {MIR_BT, "l r", "b5000000:ff000000 rd1 l"},  /* cbnz rd,l */
  {MIR_BTS, "l r", "35000000:ff000000 rd1 l"}, /* cbnz wd,l */
  {MIR_BF, "l r", "b4000000:ff000000 rd1 l"},  /* cbz rd,l */
  {MIR_BFS, "l r", "34000000:ff000000 rd1 l"}, /* cbz wd,l */

  {MIR_BO, "l", "54000006:ff00001f l"},  /* b.vs */
  {MIR_UBO, "l", "54000002:ff00001f l"}, /* b.cs */

  {MIR_BNO, "l", "54000007:ff00001f l"},  /* b.vc */
  {MIR_UBNO, "l", "54000003:ff00001f l"}, /* b.cc */

#define BEQ "54000000:ff00001f l"
  // ??? add extended reg cmp insns:
  // all ld insn are changed to builtins and bt/bts
  /* cmp Rn,Rm; beq l */
  {MIR_BEQ, "l r r", CMPR "; " BEQ},
  /* cmp Rn,I,shift ; beq l */
  {MIR_BEQ, "l r I", CMPI "; " BEQ},
  /* cmp Wn,Wm;beq l */
  {MIR_BEQS, "l r r", SCMPR "; " BEQ},
  /* cmp Wn,I,shift; beq l */
  {MIR_BEQS, "l r I", SCMPI "; " BEQ},
  /* fcmpe Sn,Sm; beq l */
  {MIR_FBEQ, "l r r", FCMP "; " BEQ},
  /* fcmpe Dn,Dm; beq l */
  {MIR_DBEQ, "l r r", DCMP "; " BEQ},
  /* fcmpe Sn,0.0; beq l */
  {MIR_FBEQ, "l r Zf", "1e202018:fffffc1f vn1 vm2; " BEQ},
  /* fcmpe Dn,0.0; beq l */
  {MIR_DBEQ, "l r Zd", "1e602018:fffffc1f vn1 vm2; " BEQ},

#define BNE "54000001:ff00001f l"
  /* cmp Rn,Rm; bne l */
  {MIR_BNE, "l r r", CMPR "; " BNE},
  /* cmp Rn,I,shift ; bne l */
  {MIR_BNE, "l r I", CMPI "; " BNE},
  /* cmp Wn,Wm;bne l */
  {MIR_BNES, "l r r", SCMPR "; " BNE},
  /* cmp Wn,I,shift; bne l */
  {MIR_BNES, "l r I", SCMPI "; " BNE},
  /* fcmpe Sn,Sm; bne l */
  {MIR_FBNE, "l r r", FCMP "; " BNE},
  /* fcmpe Dn,Dm; bne l */
  {MIR_DBNE, "l r r", DCMP "; " BNE},
  /* fcmpe Sn,0.0; bne l */
  {MIR_FBNE, "l r Zf", "1e202018:fffffc1f vn1 vm2; " BNE},
  /* fcmpe Dn,0.0; bne l */
  {MIR_DBNE, "l r Zd", "1e602018:fffffc1f vn1 vm2; " BNE},

#define BLT "5400000b:ff00001f l"
#define UBLT "54000003:ff00001f l"
  /* cmp Rn,Rm; blt l */
  {MIR_BLT, "l r r", CMPR "; " BLT},
  /* cmp Rn,I,shift ; blt l */
  {MIR_BLT, "l r I", CMPI "; " BLT},
  /* cmp Wn,Wm;blt l */
  {MIR_BLTS, "l r r", SCMPR "; " BLT},
  /* cmp Wn,I,shift; blt l */
  {MIR_BLTS, "l r I", SCMPI "; " BLT},
  /* cmp Rn,Rm; bcc l */
  {MIR_UBLT, "l r r", CMPR "; " UBLT},
  /* cmp Rn,I,shift ; bcc l */
  {MIR_UBLT, "l r I", CMPI "; " UBLT},
  /* cmp Wn,Wm;bcc l */
  {MIR_UBLTS, "l r r", SCMPR "; " UBLT},
  /* cmp Wn,I,shift; bcc l */
  {MIR_UBLTS, "l r I", SCMPI "; " UBLT},

#define BGE "5400000a:ff00001f l"
#define UBGE "54000002:ff00001f l"
  /* cmp Rn,Rm; bge l */
  {MIR_BGE, "l r r", CMPR "; " BGE},
  /* cmp Rn,I,shift ; bge l */
  {MIR_BGE, "l r I", CMPI "; " BGE},
  /* cmp Wn,Wm;bge l */
  {MIR_BGES, "l r r", SCMPR "; " BGE},
  /* cmp Wn,I,shift; bge l */
  {MIR_BGES, "l r I", SCMPI "; " BGE},
  /* cmp Rn,Rm; bcs l */
  {MIR_UBGE, "l r r", CMPR "; " UBGE},
  /* cmp Rn,I,shift ; bcs l */
  {MIR_UBGE, "l r I", CMPI "; " UBGE},
  /* cmp Wn,Wm;bcs l */
  {MIR_UBGES, "l r r", SCMPR "; " UBGE},
  /* cmp Wn,I,shift; bcs l */
  {MIR_UBGES, "l r I", SCMPI "; " UBGE},
  /* fcmpe Sn,Sm; bpl l */
  {MIR_FBGE, "l r r", FCMP "; " BGE},
  /* fcmpe Dn,Dm; bpl l */
  {MIR_DBGE, "l r r", DCMP "; " BGE},
  /* fcmpe Sn,0.0; bpl l */
  {MIR_FBGE, "l r Zf", "1e202018:fffffc1f vn1 vm2; " BGE},
  /* fcmpe Dn,0.0; bpl l */
  {MIR_DBGE, "l r Zd", "1e602018:fffffc1f vn1 vm2; " BGE},

#define BGT "5400000c:ff00001f l"
#define UBGT "54000008:ff00001f l"
  /* cmp Rn,Rm; bgt l */
  {MIR_BGT, "l r r", CMPR "; " BGT},
  /* cmp Rn,I,shift ; bgt l */
  {MIR_BGT, "l r I", CMPI "; " BGT},
  /* cmp Wn,Wm;bgt l */
  {MIR_BGTS, "l r r", SCMPR "; " BGT},
  /* cmp Wn,I,shift; bgt l */
  {MIR_BGTS, "l r I", SCMPI "; " BGT},
  /* cmp Rn,Rm; bhi l */
  {MIR_UBGT, "l r r", CMPR "; " UBGT},
  /* cmp Rn,I,shift ; bhi l */
  {MIR_UBGT, "l r I", CMPI "; " UBGT},
  /* cmp Wn,Wm;bhi l */
  {MIR_UBGTS, "l r r", SCMPR "; " UBGT},
  /* cmp Wn,I,shift; bhi l */
  {MIR_UBGTS, "l r I", SCMPI "; " UBGT},
  /* fcmpe Sn,Sm; bhi l */
  {MIR_FBGT, "l r r", FCMP "; " BGT},
  /* fcmpe Dn,Dm; bhi l */
  {MIR_DBGT, "l r r", DCMP "; " BGT},
  /* fcmpe Sn,0.0; bhi l */
  {MIR_FBGT, "l r Zf", "1e202018:fffffc1f vn1 vm2; " BGT},
  /* fcmpe Dn,0.0; bhi l */
  {MIR_DBGT, "l r Zd", "1e602018:fffffc1f vn1 vm2; " BGT},

#define BLE "5400000d:ff00001f l"
#define UBLE "54000009:ff00001f l"
  /* cmp Rn,Rm; ble l */
  {MIR_BLE, "l r r", CMPR "; " BLE},
  /* cmp Rn,I,shift ; ble l */
  {MIR_BLE, "l r I", CMPI "; " BLE},
  /* cmp Wn,Wm;ble l */
  {MIR_BLES, "l r r", SCMPR "; " BLE},
  /* cmp Wn,I,shift; ble l */
  {MIR_BLES, "l r I", SCMPI "; " BLE},
  /* cmp Rn,Rm; bls l */
  {MIR_UBLE, "l r r", CMPR "; " UBLE},
  /* cmp Rn,I,shift ; bls l */
  {MIR_UBLE, "l r I", CMPI "; " UBLE},
  /* cmp Wn,Wm;bls l */
  {MIR_UBLES, "l r r", SCMPR "; " UBLE},
  /* cmp Wn,I,shift; bls l */
  {MIR_UBLES, "l r I", SCMPI "; " UBLE},

  // ??? with shift
  {MIR_NEG, "r r", "cb0003e0:ff2003e0 rd0 rm1"},  /* neg Rd,Rm */
  {MIR_NEGS, "r r", "4b0003e0:ff2003e0 rd0 rm1"}, /* neg Wd,Wm */
  {MIR_FNEG, "r r", "1e214000:fffffc00 vd0 vn1"}, /* fneg Sd,Sn */
  {MIR_DNEG, "r r", "1e614000:fffffc00 vd0 vn1"}, /* fneg Dd,Dn */
  // ldneg is a builtin

  {MIR_LSH, "r r r", "9ac02000:ffe0fc00 rd0 rn1 rm2"},  /* lsl Rd,Rn,Rm */
  {MIR_LSHS, "r r r", "1ac02000:ffe0fc00 rd0 rn1 rm2"}, /* lsl Wd,Wn,Wm */
  {MIR_LSH, "r r SL", "d3400000:ffc00000 rd0 rn1 SL"},  /* ubfm Rd,Rn,immr,imms */
  {MIR_LSHS, "r r Sl", "53000000:ffc00000 rd0 rn1 Sl"}, /* ubfm Wd,Wn,immr,imms */

  {MIR_RSH, "r r r", "9ac02800:ffe0fc00 rd0 rn1 rm2"},  /* asr Rd,Rn,Rm */
  {MIR_RSHS, "r r r", "1ac02800:ffe0fc00 rd0 rn1 rm2"}, /* asr Wd,Wn,Wm */
  {MIR_RSH, "r r SR", "9340fc00:ffc0fc00 rd0 rn1 S"},   /* asr Rd,Rn,S */
  {MIR_RSHS, "r r Sr", "13007c00:ffc0fc00 rd0 rn1 S"},  /* asr Wd,Wn,S */

  {MIR_URSH, "r r r", "9ac02400:ffe0fc00 rd0 rn1 rm2"},  /* lsr Rd,Rn,Rm */
  {MIR_URSHS, "r r r", "1ac02400:ffe0fc00 rd0 rn1 rm2"}, /* lsr Wd,Wn,Wm */
  {MIR_URSH, "r r SR", "d340fc00:ffc0fc00 rd0 rn1 S"},   /* lsr Rd,Rn,S */
  {MIR_URSHS, "r r Sr", "53007c00:ffc0fc00 rd0 rn1 S"},  /* lsr Wd,Wn,S */

  // ??? adding shift, negate, immediate
  {MIR_AND, "r r r", "8a000000:ffe0fc00 rd0 rn1 rm2"},  /* and Rd,Rn,Rm */
  {MIR_ANDS, "r r r", "0a000000:ffe0fc00 rd0 rn1 rm2"}, /* and Wd,Wn,Wm */

  {MIR_OR, "r r r", "aa000000:ffe0fc00 rd0 rn1 rm2"},  /* orr Rd,Rn,Rm */
  {MIR_ORS, "r r r", "2a000000:ffe0fc00 rd0 rn1 rm2"}, /* orr Wd,Wn,Wm */

  {MIR_XOR, "r r r", "ca000000:ffe0fc00 rd0 rn1 rm2"},  /* eor Rd,Rn,Rm */
  {MIR_XORS, "r r r", "4a000000:ffe0fc00 rd0 rn1 rm2"}, /* eor Wd,Wn,Wm */

  // ??? can we add scale
  {MIR_I2F, "r r", "9e220000:ffff0000 vd0 rn1"},  /* scvtf Sd,Rn */
  {MIR_I2D, "r r", "9e620000:ffff0000 vd0 rn1"},  /* scvtf Dd,Rn */
  {MIR_UI2F, "r r", "9e230000:ffff0000 vd0 rn1"}, /* ucvtf Sd,Rn */
  {MIR_UI2D, "r r", "9e630000:ffff0000 vd0 rn1"}, /* ucvtf Dd,Rn */
  {MIR_F2I, "r r", "9e380000:ffff0000 rd0 vn1"},  /* fcvtzs Rd,Sn */
  {MIR_D2I, "r r", "9e780000:ffff0000 rd0 vn1"},  /* fcvtzs Rd,Dn */
  {MIR_F2D, "r r", "1e22c000:fffffc00 vd0 vn1"},  /* fcvt Dd,Sn */
  {MIR_D2F, "r r", "1e624000:fffffc00 vd0 vn1"},  /* fcvt Sd,Dn */
  // i2ld, ui2ld, ld2i, f2ld, d2ld, ld2f, ld2d are builtins

  {MIR_CALL, "X r $", "d63f0000:fffffc1f rn1"},   /* blr *Rn */
  {MIR_CALL, "X L $", "94000000:fc000000 rn1"},   /* bl address */
  {MIR_INLINE, "X r $", "d63f0000:fffffc1f rn1"}, /* blr *Rn */
  {MIR_INLINE, "X L $", "94000000:fc000000 rn1"}, /* bl address */
  {MIR_RET, "$", "d65f0000:fffffc1f hn1e"},       /* ret R30  */

  {MIR_JCALL, "X r $", "d61f0000:fffffc00 rn1"}, /* br r1 */
  {MIR_JRET, "r $", "d61f0000:fffffc00 rn0"},    /* br r0  */

  /* add r0, r1, 15; and r0, r0, -16; sub sp, sp, r0; mov r0, sp: */
  {MIR_ALLOCA, "r r",
   "91003c00:fffffc00 rd0 rn1; 927cec00:fffffc00 rd0 rn0;"         /* add r0,r1,15;and r0,r0,-16
                                                                    */
   "cb206000:ffe0fc00 hn1f hd1f rm0; 91000000:fffffc00 rd0 hn1f"}, /* sub sp,sp,r0; mov r0,sp */
  /* sub sp, sp, roundup (imm, 16); mov r0, sp: */
  {MIR_ALLOCA, "r Iu", "d1000000:ff000000 hd1f hn1f Iu; 91000000:fffffc00 rd0 hn1f"},

  {MIR_BSTART, "r", "91000000:fffffc00 rd0 hn1f"}, /* Rd = sp */
  {MIR_BEND, "r", "91000000:fffffc00 hd1f rn0"},   /* sp = Rn */

  /* adr r10,PC-relative TableAddress; ldr r10,(r10,r,8);br r10; TableContent
     We use r10 as r9 can be used if switch operand is memory. */
  {MIR_SWITCH, "r $",
   "10000000:ff000000 hda T; f8607800:ffe0fc00 hda hna rm0; d61f0000:fffffc00 hna;"},

  /* Used only during machine code generation.  Should have the same format as branch on overflow
     insns */
  /* unsigned sub sets up carry flag when there is no overflow: */
  {SUB_UBO, "l", "54000003:ff00001f l"},  /* b.cc */
  {SUB_UBNO, "l", "54000002:ff00001f l"}, /* b.cs */

  /* MULOS:smull Rd,Wn,Wm; asr r10,Rd,32; cmp W10,Wd,asr 31 */
  {MIR_MULOS, "r r r",
   "9b207c00:ffe0fc00 rd0 rn1 rm2; 9340fc00:ffc0fc00 hda rn0 I20; "
   "6b80001f:ffe0001f hna rm0 i1f"},
  /* UMULOS:umull Rd,Wn,Wm; cmp xzr,Rd,lsr 32 */
  {MIR_UMULOS, "r r r", "9ba07c00:ffe0fc00 rd0 rn1 rm2; eb40001f:ffe0001f hn1f rm0 i20"},
  /* MULO:smulh h11,Rn,Rm; mul Rd,Rn,Rm; cmp h11,Rd,asr 63 (r11 is a scratch reg) */
  {MIR_MULO, "r r r",
   "9b407c00:ffe0fc00 hdb rn1 rm2; 9b007c00:ffe0fc00 rd0 rn1 rm2; "
   "eb80001f:ffe0001f hnb rm0 i3f"},
  /* UMULO:umulh h11,Rn,Rm; mul Rd,Rn,Rm; cmp xzr,h11 (r11 is a scratch reg) */
  {MIR_UMULO, "r r r",
   "9bc07c00:ffe0fc00 hdb rn1 rm2; 9b007c00:ffe0fc00 rd0 rn1 rm2; "
   "eb00001f:ff20001f hn1f hmb"},

  /* [u]mulo[s] insns uses zero flag to check overflow: */
  {MUL_BO, "l", BNE},  /* b.ne */
  {MUL_BNO, "l", BEQ}, /* b.eq */
};

static void target_get_early_clobbered_hard_regs (MIR_insn_t insn, MIR_reg_t *hr1, MIR_reg_t *hr2) {
  *hr1 = *hr2 = MIR_NON_VAR;
  if (insn->code == MIR_MOD || insn->code == MIR_MODS || insn->code == MIR_UMOD
      || insn->code == MIR_UMODS)
    *hr1 = R8_HARD_REG;
  else if (insn->code == MIR_MULO || insn->code == MIR_UMULO)
    *hr1 = R11_HARD_REG;
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
  for (i = 0; i < ARM_INSN_BOUND; i++) VARR_PUSH (insn_pattern_info_t, insn_pattern_info, pinfo);
  info_addr = VARR_ADDR (insn_pattern_info_t, insn_pattern_info);
  for (prev_code = ARM_INSN_BOUND, i = 0; i < n; i++) {
    ind = VARR_GET (int, pattern_indexes, i);
    if ((code = patterns[ind].code) != prev_code) {
      if (i != 0) info_addr[prev_code].num = i - info_addr[prev_code].start;
      info_addr[code].start = i;
      prev_code = code;
    }
  }
  assert (prev_code != ARM_INSN_BOUND);
  info_addr[prev_code].num = n - info_addr[prev_code].start;
}

struct imm {
  int v, shift;
};

/* Return number of insn mov{n|z} movk* to express constant V. Return immediates with their shifts
   for mov{n|z}, movk in IMMS. */
static int movnzk_const (uint64_t v, int n_p, struct imm *imms) {
  int i16, shift, n = 0;

  if (n_p) v = ~v;
  if (v == 0) {
    imms[0].v = 0;
    imms[0].shift = 0;
    return 1;
  }
  for (shift = 0; v != 0; v >>= 16, shift += 16) {
    for (; (i16 = v & 0xffff) == 0; shift += 16) v >>= 16;
    gen_assert (n < 4);
    imms[n].v = n_p && n != 0 ? (~i16 & 0xffff) : i16;
    imms[n++].shift = shift;
  }
  return n;
}

/* Return shift flag (0 or 1) for arithm insn 12-bit immediate. If V cannot be represented, return
   -1. */
static int arithm_const (uint64_t v, int *imm) {
  if (v < (1 << 12)) {
    *imm = v;
    return 0;
  }
  if ((v & 0xfff) == 0 && (v >> 12) < (1 << 12)) {
    *imm = v >> 12;
    return 1;
  }
  return -1;
}

/* Return shift flag (0 or 1) for arithm insn 12-bit immediate rounded
   up to 16. If the rounded V cannot be represented, return -1. */
static int arithm_roundup_const (uint64_t v, int *imm) {
  return arithm_const ((v + 15) / 16 * 16, imm);
}

/* Return immr for right 64-bit or 32-bit (if SHORT_P) shift by V.  If the
   shift can not be represented, return FALSE. */
static int rshift_const (int64_t v, int short_p) {
  return v < 0 || v > 63 || (short_p && v > 31) ? -1 : v;
}

/* Return immr and imms for left 64-bit or 32-bit (if SHORT_P) shift
   by V.  If the shift can not be represented, return FALSE. */
static int lshift_const_p (int64_t v, int short_p, int *immr, int *imms) {
  if (short_p) {
    if (v < 0 || v > 31) return FALSE;
    *immr = (-v) & 0x1f;
    *imms = 31 - v;
  } else {
    if (v < 0 || v > 63) return FALSE;
    *immr = (-v) & 0x3f;
    *imms = 63 - v;
  }
  return TRUE;
}

static int pattern_match_p (gen_ctx_t gen_ctx, const struct pattern *pat, MIR_insn_t insn) {
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
      int scale, u_p, s_p;

      if (op.mode != MIR_OP_VAR_MEM) return FALSE;
      u_p = s_p = TRUE;
      ch = *++p;
      switch (ch) {
      case 'f':
        type = MIR_T_F;
        type2 = MIR_T_BOUND;
        scale = 4;
        break;
      case 'd':
        type = MIR_T_D;
        type2 = MIR_T_BOUND;
        scale = 8;
        break;
      case 'l':
        ch = *++p;
        gen_assert (ch == 'd');
        type = MIR_T_LD;
        type2 = MIR_T_BOUND;
        scale = 16;
        break;
      case 'u':
      case 's':
        u_p = ch == 'u';
        s_p = ch == 's';
        ch = *++p;
        /* fall through */
      default:
        gen_assert ('0' <= ch && ch <= '3');
        scale = 1 << (ch - '0');
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
      if (start_ch == 'm'
          && (op.u.var_mem.disp != 0
              || (op.u.var_mem.index != MIR_NON_VAR && op.u.var_mem.scale != 1
                  && op.u.var_mem.scale != scale)))
        return FALSE;
      if (start_ch == 'M'
          && (op.u.var_mem.index != MIR_NON_VAR || op.u.var_mem.disp < 0
              || op.u.var_mem.disp % scale != 0 || op.u.var_mem.disp / scale >= (1 << 12)))
        return FALSE;
      break;
    }
    case 'Z':
    case 'N': {
      int n;
      uint64_t v;
      struct imm imms[4];

      ch = *++p;
      if (ch == 'f' && op.mode == MIR_OP_FLOAT) {
        if (op.u.f != 0.0f) return FALSE;
      } else if (ch == 'd' && op.mode == MIR_OP_DOUBLE) {
        if (op.u.d != 0.0) return FALSE;
      } else {
        if (op.mode != MIR_OP_INT && op.mode != MIR_OP_UINT && op.mode != MIR_OP_REF) return FALSE;
        gen_assert (('0' <= ch && ch <= '2') || (start_ch == 'Z' && ch == '3'));
        n = ch - '0';
        if (op.mode != MIR_OP_REF) {
          v = op.u.u;
        } else if (op.u.ref->item_type == MIR_data_item && op.u.ref->u.data->name != NULL
                   && _MIR_reserved_ref_name_p (ctx, op.u.ref->u.data->name)) {
          v = (uint64_t) op.u.ref->u.data->u.els;
        } else {
          v = (uint64_t) op.u.ref->addr;
        }
        if (movnzk_const (v, start_ch == 'N', imms) > n + 1) return FALSE;
        gen_assert (nop == 1); /* only 2nd move operand */
      }
      break;
    }
    case 'I': {
      int imm;

      if (op.mode != MIR_OP_INT && op.mode != MIR_OP_UINT) return FALSE;
      ch = *++p;
      if (ch == 'u') {
        if (arithm_roundup_const (op.u.u, &imm) < 0) return FALSE;
      } else {
        p--;
        if (arithm_const (op.u.u, &imm) < 0) return FALSE;
      }
      break;
    }
    case 'S': {
      int immr, imms;

      if (op.mode != MIR_OP_INT && op.mode != MIR_OP_UINT) return FALSE;
      gen_assert (op.mode != MIR_OP_INT || op.u.i >= 0);
      ch = *++p;
      if (ch == 'r' || ch == 'R') {
        if ((op.mode == MIR_OP_UINT && op.u.i < 0) || rshift_const (op.u.i, ch == 'r') < 0)
          return FALSE;
      } else {
        gen_assert (ch == 'l' || ch == 'L');
        if ((op.mode == MIR_OP_UINT && op.u.i < 0)
            || !lshift_const_p (op.u.i, ch == 'l', &immr, &imms))
          return FALSE;
      }
      break;
    }
    case 'l':
      if (op.mode != MIR_OP_LABEL) return FALSE;
      break;
    case 'L':
      if (op.mode != MIR_OP_LABEL && op.mode != MIR_OP_REF) return FALSE;
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
  insn_pattern_info_t info;
  int code = insn->code;

  if (code == MIR_BO || code == MIR_BNO || code == MIR_UBO || code == MIR_UBNO) {
    for (MIR_insn_t prev_insn = DLIST_PREV (MIR_insn_t, insn); prev_insn != NULL;
         prev_insn = DLIST_PREV (MIR_insn_t, prev_insn)) {
      if (prev_insn->code == MIR_SUBOS || prev_insn->code == MIR_SUBO) {
        /* unsigned sub sets up carry flag when there is no overflow: */
        if (code == MIR_UBO || code == MIR_UBNO) code = code == MIR_UBO ? SUB_UBO : SUB_UBNO;
        break;
      } else if (prev_insn->code == MIR_MULOS || prev_insn->code == MIR_MULO
                 || prev_insn->code == MIR_UMULOS || prev_insn->code == MIR_UMULO) {
        /* [u]mulo[s] insns uses zero flag to check overflow: */
        code = code == MIR_BO || code == MIR_UBO ? MUL_BO : MUL_BNO;
        break;
      } else if (prev_insn->code == MIR_ADDOS || prev_insn->code == MIR_ADDO
                 || prev_insn->code == MIR_LABEL || MIR_branch_code_p (prev_insn->code)) {
        break;
      }
    }
  }
  info = VARR_GET (insn_pattern_info_t, insn_pattern_info, code);
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

static int hex_value (int ch) {
  return ('0' <= ch && ch <= '9'   ? ch - '0'
          : 'A' <= ch && ch <= 'F' ? ch - 'A' + 10
          : 'a' <= ch && ch <= 'f' ? ch - 'a' + 10
                                   : -1);
}

static uint64_t read_hex (const char **ptr) {
  int v;
  const char *p;
  uint64_t res = 0;

  for (p = *ptr; (v = hex_value (*p)) >= 0; p++) {
    gen_assert ((res >> 60) == 0);
    res = res * 16 + v;
  }
  gen_assert (p != *ptr);
  *ptr = p - 1;
  return res;
}

static void put_byte (struct gen_ctx *gen_ctx, int byte) { VARR_PUSH (uint8_t, result_code, byte); }

static void put_uint64 (struct gen_ctx *gen_ctx, uint64_t v, int nb) {
  for (; nb > 0; nb--) {
    put_byte (gen_ctx, v & 0xff);
    v >>= 8;
  }
}

static void set_int64 (uint8_t *addr, int64_t v, int nb) { /* Little endian */
  for (; nb > 0; nb--) {
    *addr++ = v & 0xff;
    v >>= 8;
  }
}

static int64_t get_int64 (uint8_t *addr, int nb) { /* Little endian */
  int64_t v = 0;
  int i, sh = (8 - nb) * 8;

  for (i = nb - 1; i >= 0; i--) v = (v << 8) | addr[i];
  if (sh > 0) v = (v << sh) >> sh; /* make it signed */
  return v;
}

static uint32_t check_and_set_mask (uint32_t opcode_mask, uint32_t mask) {
  gen_assert ((opcode_mask & mask) == 0);
  return opcode_mask | mask;
}

static void out_insn (gen_ctx_t gen_ctx, MIR_insn_t insn, const char *replacement,
                      void **jump_addrs) {
  MIR_context_t ctx = gen_ctx->ctx;
  size_t offset;
  const char *p, *insn_str;
  label_ref_t lr;
  int switch_table_adr_insn_start = -1;

  if (insn->code == MIR_ALLOCA
      && (insn->ops[1].mode == MIR_OP_INT || insn->ops[1].mode == MIR_OP_UINT))
    insn->ops[1].u.u = (insn->ops[1].u.u + 15) & -16;
  for (insn_str = replacement;; insn_str = p + 1) {
    char ch, ch2, start_ch, d;
    uint32_t opcode = 0, opcode_mask = 0xffffffff;
    int rd = -1, rn = -1, rm = -1, ra = -1, disp = -1, scale = -1;
    int immr = -1, imms = -1, imm16 = -1, imm16_shift = -1, imm12 = -1, imm12_shift = -1;
    MIR_op_t op;
    int label_ref_num = -1, switch_table_addr_p = FALSE;

    for (p = insn_str; (ch = *p) != '\0' && ch != ';'; p++) {
      if ((d = hex_value (ch = *p)) >= 0) { /* opcode and mask */
        gen_assert (opcode == 0 && opcode_mask == 0xffffffff);
        do {
          opcode = opcode * 16 + d;
          p++;
        } while ((d = hex_value (*p)) >= 0);
        if ((ch = *p) == ':') {
          p++;
          opcode_mask = 0;
          while ((d = hex_value (ch = *p)) >= 0) {
            opcode_mask = opcode_mask * 16 + d;
            p++;
          }
        }
        gen_assert ((opcode & ~opcode_mask) == 0);
      }
      if ((ch = *p) == 0 || ch == ';') break;
      switch ((start_ch = ch = *p)) {
      case ' ':
      case '\t': break;
      case 'r':
      case 'v':
      case 'h': {
        int reg;

        ch2 = *++p;
        gen_assert (ch2 == 'd' || ch2 == 'n' || ch2 == 'm'
                    || (ch2 == 'a'
                        && (insn->code == MIR_MOD || insn->code == MIR_MODS
                            || insn->code == MIR_UMOD || insn->code == MIR_UMODS)));
        ch = *++p;
        if (start_ch == 'h') {
          reg = read_hex (&p);
        } else {
          gen_assert ('0' <= ch && ch <= '2' && ch - '0' < (int) insn->nops);
          op = insn->ops[ch - '0'];
          gen_assert (op.mode == MIR_OP_VAR);
          reg = op.u.var;
          if (start_ch != 'v') {
            gen_assert (reg < V0_HARD_REG);
          } else {
            gen_assert (reg >= V0_HARD_REG);
            reg -= V0_HARD_REG;
          }
        }
        gen_assert (reg <= 31);
        if (ch2 == 'd')
          rd = reg;
        else if (ch2 == 'n')
          rn = reg;
        else if (ch2 == 'm')
          rm = reg;
        else
          ra = reg;
        break;
      }
      case 'm':
        op = insn->ops[0].mode == MIR_OP_VAR_MEM ? insn->ops[0] : insn->ops[1];
        rn = op.u.var_mem.base;
        rm = op.u.var_mem.index == MIR_NON_VAR ? ZR_HARD_REG : op.u.var_mem.index;
        scale = op.u.var_mem.scale;
        break;
      case 'M': {
        int dsize = 1;

        op = insn->ops[0].mode == MIR_OP_VAR_MEM ? insn->ops[0] : insn->ops[1];
        switch (op.u.var_mem.type) {
        case MIR_T_I8:
        case MIR_T_U8: dsize = 1; break;
        case MIR_T_I16:
        case MIR_T_U16: dsize = 2; break;
#if MIR_PTR32
        case MIR_T_P:
#endif
        case MIR_T_I32:
        case MIR_T_U32:
        case MIR_T_F: dsize = 4; break;
#if MIR_PTR64
        case MIR_T_P:
#endif
        case MIR_T_I64:
        case MIR_T_U64:
        case MIR_T_D: dsize = 8; break;
        case MIR_T_LD: dsize = 16; break;
        default: assert (FALSE);
        }
        gen_assert (op.u.var_mem.disp % dsize == 0);
        rn = op.u.var_mem.base;
        disp = op.u.var_mem.disp / dsize;
        gen_assert (disp < (1 << 12));
        break;
      }
      case 'S': { /* S, SL, Sl */
        int flag;

        op = insn->ops[2];
        gen_assert (op.mode == MIR_OP_INT || op.mode == MIR_OP_UINT);
        ch = *++p;
        if (ch == 'L' || ch == 'l') {
          flag = lshift_const_p (op.u.i, ch == 'l', &immr, &imms);
          gen_assert (flag);
        } else {
          p--;
          immr = rshift_const (op.u.i, FALSE);
          gen_assert (immr >= 0);
        }
        break;
      }
      case 'N':
      case 'Z': {
        int n, n2;
        uint64_t v;
        struct imm immediates[4];

        ch = *++p;
        gen_assert ('0' <= ch && ch <= '3');
        op = insn->ops[1];
        n = ch - '0';
        if (op.mode != MIR_OP_REF) {
          v = op.u.u;
        } else if (op.u.ref->item_type == MIR_data_item && op.u.ref->u.data->name != NULL
                   && _MIR_reserved_ref_name_p (ctx, op.u.ref->u.data->name)) {
          v = (uint64_t) op.u.ref->u.data->u.els;
        } else {
          v = (uint64_t) op.u.ref->addr;
        }
        n2 = movnzk_const (v, start_ch == 'N', immediates);
        gen_assert (n < n2);
        imm16 = immediates[n].v;
        imm16_shift = immediates[n].shift >> 4;
        break;
      }
      case 'I': {
        ch = *++p;
        if (ch == 'u') { /* Iu */
          op = insn->ops[1];
          gen_assert (op.mode == MIR_OP_INT || op.mode == MIR_OP_UINT);
          imm12_shift = arithm_roundup_const (op.u.u, &imm12);
        } else if (hex_value (ch) >= 0) {
          immr = read_hex (&p);
        } else { /* I */
          op = insn->ops[2];
          gen_assert (op.mode == MIR_OP_INT || op.mode == MIR_OP_UINT);
          imm12_shift = arithm_const (op.u.u, &imm12);
          p--;
        }
        break;
      }
      case 'i': {
        p++;
        gen_assert (hex_value (*p) >= 0);
        imms = read_hex (&p);
        break;
      }
      case 'T': {
        gen_assert (!switch_table_addr_p && switch_table_adr_insn_start < 0);
        switch_table_addr_p = TRUE;
        break;
      }
      case 'l':
      case 'L': {
        int nop = 0;
        if (insn->code == MIR_LADDR || insn->code == MIR_CALL || insn->code == MIR_INLINE) nop = 1;
        op = insn->ops[nop];
        gen_assert (op.mode == MIR_OP_LABEL || (start_ch == 'L' && op.mode == MIR_OP_REF));
        lr.abs_addr_p = FALSE;
        lr.short_p = start_ch == 'l';
        lr.label_val_disp = 0;
        if (jump_addrs == NULL)
          lr.u.label = op.u.label;
        else
          lr.u.jump_addr = jump_addrs[0];
        label_ref_num = VARR_LENGTH (label_ref_t, label_refs);
        VARR_PUSH (label_ref_t, label_refs, lr);
        break;
      }
      default: gen_assert (FALSE);
      }
    }

    if (rd >= 0) {
      gen_assert (rd <= 31);
      opcode |= rd;
      opcode_mask = check_and_set_mask (opcode_mask, 0x1f);
    }
    if (rn >= 0) {
      gen_assert (rn <= 31);
      opcode |= rn << 5;
      opcode_mask = check_and_set_mask (opcode_mask, 0x1f << 5);
    }
    if (rm >= 0) {
      gen_assert (rm <= 31);
      opcode |= rm << 16;
      opcode_mask = check_and_set_mask (opcode_mask, 0x1f << 16);
    }
    if (ra >= 0) {
      gen_assert (rm <= 31);
      opcode |= ra << 10;
      opcode_mask = check_and_set_mask (opcode_mask, 0x1f << 10);
    }
    if (scale >= 0) {
      opcode |= (scale == 1 ? 0x6 : 0x7) << 12;
      opcode_mask = check_and_set_mask (opcode_mask, 0xf << 12);
    }
    if (disp >= 0) {
      gen_assert (disp < (1 << 12));
      opcode |= disp << 10;
      opcode_mask = check_and_set_mask (opcode_mask, 0xfff << 10);
    }
    if (immr >= 0) {
      gen_assert (immr < (1 << 6));
      opcode |= immr << 16;
      opcode_mask = check_and_set_mask (opcode_mask, 0x3f << 16);
    }
    if (imms >= 0) {
      gen_assert (imms < (1 << 6));
      opcode |= imms << 10;
      opcode_mask = check_and_set_mask (opcode_mask, 0x3f << 10);
    }
    if (imm16 >= 0) {
      gen_assert (imm16 < (1 << 16));
      opcode |= imm16 << 5;
      opcode_mask = check_and_set_mask (opcode_mask, 0xffff << 5);
    }
    if (imm16_shift >= 0) {
      gen_assert (imm16_shift < (1 << 2));
      opcode |= imm16_shift << 21;
      opcode_mask = check_and_set_mask (opcode_mask, 0x3 << 21);
    }
    if (imm12 >= 0) {
      gen_assert (imm12 < (1 << 12));
      opcode |= imm12 << 10;
      opcode_mask = check_and_set_mask (opcode_mask, 0xfff << 10);
    }
    if (imm12_shift >= 0) {
      gen_assert (imm12_shift < (1 << 2));
      opcode |= imm12_shift << 22;
      opcode_mask = check_and_set_mask (opcode_mask, 0x3 << 22);
    }
    if (label_ref_num >= 0) VARR_ADDR (label_ref_t, label_refs)
    [label_ref_num].label_val_disp = VARR_LENGTH (uint8_t, result_code);

    if (switch_table_addr_p) switch_table_adr_insn_start = VARR_LENGTH (uint8_t, result_code);
    put_uint64 (gen_ctx, opcode, 4); /* output the machine insn */

    if (*p == 0) break;
  }
  if (switch_table_adr_insn_start < 0) return;
  if (VARR_LENGTH (uint8_t, result_code) % 8 == 4) put_uint64 (gen_ctx, 0, 4);
  offset = (VARR_LENGTH (uint8_t, result_code) - switch_table_adr_insn_start) / 4; /* pc offset */
  *(uint32_t *) (VARR_ADDR (uint8_t, result_code) + switch_table_adr_insn_start) |= (offset << 5);
  gen_assert (insn->code == MIR_SWITCH);
  for (size_t i = 1; i < insn->nops; i++) {
    gen_assert (insn->ops[i].mode == MIR_OP_LABEL);
    lr.abs_addr_p = TRUE;
    lr.short_p = FALSE;
    lr.label_val_disp = VARR_LENGTH (uint8_t, result_code);
    if (jump_addrs == NULL)
      lr.u.label = insn->ops[i].u.label;
    else
      lr.u.jump_addr = jump_addrs[i - 1];
    VARR_PUSH (label_ref_t, label_refs, lr);
    put_uint64 (gen_ctx, 0, 8);
  }
}

static int target_memory_ok_p (gen_ctx_t gen_ctx, MIR_op_t *op_ref) {
  gen_assert (op_ref->mode == MIR_OP_VAR_MEM);
  MIR_context_t ctx = gen_ctx->ctx;
  size_t size = _MIR_type_size (ctx, op_ref->u.var_mem.type);
  int scale = gen_int_log2 ((int64_t) size);

  if (op_ref->u.var_mem.disp == 0
      && ((op_ref->u.var_mem.index == MIR_NON_VAR || op_ref->u.var_mem.scale == 1
           || op_ref->u.var_mem.scale == scale)))
    return TRUE;
  if (op_ref->u.var_mem.index == MIR_NON_VAR && op_ref->u.var_mem.disp >= 0
      && op_ref->u.var_mem.disp % scale == 0 && op_ref->u.var_mem.disp / scale < (1 << 12))
    return TRUE;
  return FALSE;
}

static int target_insn_ok_p (gen_ctx_t gen_ctx, MIR_insn_t insn) {
  return find_insn_pattern_replacement (gen_ctx, insn) != NULL;
}

static void target_split_insns (gen_ctx_t gen_ctx MIR_UNUSED) {}

static uint8_t *target_translate (gen_ctx_t gen_ctx, size_t *len) {
  MIR_context_t ctx = gen_ctx->ctx;
  size_t i;
  MIR_insn_t insn;
  const char *replacement;

  gen_assert (curr_func_item->item_type == MIR_func_item);
  VARR_TRUNC (uint8_t, result_code, 0);
  VARR_TRUNC (label_ref_t, label_refs, 0);
  VARR_TRUNC (uint64_t, abs_address_locs, 0);
  for (insn = DLIST_HEAD (MIR_insn_t, curr_func_item->u.func->insns); insn != NULL;
       insn = DLIST_NEXT (MIR_insn_t, insn)) {
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

    if (!lr.abs_addr_p) {
      int64_t offset = (int64_t) get_label_disp (gen_ctx, lr.u.label)
                       - (int64_t) lr.label_val_disp; /* pc offset */
      gen_assert ((offset & 0x3) == 0);
      if (lr.short_p)
        *(uint32_t *) (VARR_ADDR (uint8_t, result_code) + lr.label_val_disp)
          |= ((offset / 4) & 0x7ffff) << 5; /* 19-bit */
      else
        *(uint32_t *) (VARR_ADDR (uint8_t, result_code) + lr.label_val_disp)
          |= (offset / 4) & 0x3ffffff; /* 26-bit */
    } else {
      set_int64 (&VARR_ADDR (uint8_t, result_code)[lr.label_val_disp],
                 (int64_t) get_label_disp (gen_ctx, lr.u.label), 8);
      VARR_PUSH (uint64_t, abs_address_locs, lr.label_val_disp);
    }
  }
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
    reloc.value = base + get_int64 (base + reloc.offset, 8);
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
  replacement = find_insn_pattern_replacement (gen_ctx, insn);
  gen_assert (replacement != NULL);
  out_insn (gen_ctx, insn, replacement, jump_addrs);
  if (MIR_branch_code_p (insn->code) && insn->code != MIR_JMP) short_bb_branch_p = TRUE;
}

static void target_output_jump (gen_ctx_t gen_ctx, void **jump_addrs) {
  out_insn (gen_ctx, temp_jump, temp_jump_replacement, jump_addrs);
}

static uint8_t *target_bb_translate_finish (gen_ctx_t gen_ctx, size_t *len) {
  /* add nop for possible conversion short branch to branch and jump */
  if (short_bb_branch_p) put_uint64 (gen_ctx, TARGET_NOP, 4);
  while (VARR_LENGTH (uint8_t, result_code) % 16 != 0) /* Align the pool */
    VARR_PUSH (uint8_t, result_code, 0);
  *len = VARR_LENGTH (uint8_t, result_code);
  return VARR_ADDR (uint8_t, result_code);
}

static void setup_rel (gen_ctx_t gen_ctx, label_ref_t *lr, uint8_t *base, void *addr) {
  MIR_context_t ctx = gen_ctx->ctx;
  int64_t offset = (int64_t) addr - (int64_t) (base + lr->label_val_disp);

  gen_assert ((offset & 0x3) == 0);
  offset >>= 2;
  /* check max 26-bit offset with possible branch conversion (see offset - 2): */
  if (lr->abs_addr_p || !(-(1 << 25) <= (offset - 2) && offset < (1 << 25))) {
    fprintf (stderr, "too big offset (%lld) in setup_rel", (long long) offset);
    exit (1);
  }
  /* ??? thread safe: */
  uint32_t *insn_ptr = (uint32_t *) (base + lr->label_val_disp), insn = *insn_ptr;
  if (!lr->short_p) {
    insn = (insn & ~0x3ffffff) | (offset & 0x3ffffff);
  } else if (-(1 << 18) <= offset && offset < (1 << 18)) { /* 19 bit offset */
    insn = (insn & ~(0x7ffff << 5)) | ((offset & 0x7ffff) << 5);
  } else {
    insn = (insn & ~(0x7ffff << 5)) | (2 << 5); /* skip jump */
    uint32_t *nop_ptr = (uint32_t *) (base + lr->label_val_disp + 8);
    gen_assert (TARGET_NOP == *nop_ptr || (*nop_ptr & ~0x3ffffff) == 0x14000000); /* nop or jump */
    uint32_t jump_insn = 0x14000000 | ((offset - 2) & 0x3ffffff);
    _MIR_change_code (ctx, (uint8_t *) nop_ptr, (uint8_t *) &jump_insn, 4);
    lr->short_p = FALSE;
    lr->label_val_disp += 8;
  }
  _MIR_change_code (ctx, (uint8_t *) insn_ptr, (uint8_t *) &insn, 4);
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
    reloc.value = base + get_int64 (base + reloc.offset, 8);
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
  temp_jump_replacement = find_insn_pattern_replacement (gen_ctx, temp_jump);
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
