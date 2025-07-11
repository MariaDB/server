/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.

   File contains MIR interpreter which is an obligatory part of MIR API.
*/

#include "mir-alloc.h"
#include "mir.h"
#ifdef MIR_NO_INTERP
static void interp_init (MIR_context_t ctx) {}
static void finish_func_interpretation (MIR_item_t func_item, MIR_alloc_t alloc) {}
static void interp_finish (MIR_context_t ctx) {}
void MIR_interp (MIR_context_t ctx, MIR_item_t func_item, MIR_val_t *results, size_t nargs, ...) {}
void MIR_interp_arr_varg (MIR_context_t ctx, MIR_item_t func_item, MIR_val_t *results, size_t nargs,
                          MIR_val_t *vals, va_list va) {}
void MIR_interp_arr (MIR_context_t ctx, MIR_item_t func_item, MIR_val_t *results, size_t nargs,
                     MIR_val_t *vals) {}
void MIR_set_interp_interface (MIR_context_t ctx, MIR_item_t func_item) {}
#else

#ifndef MIR_INTERP_TRACE
#define MIR_INTERP_TRACE 0
#endif

#if !defined(MIR_DIRECT_DISPATCH) && defined(__GNUC__)
#define DIRECT_THREADED_DISPATCH 1
#else
#define DIRECT_THREADED_DISPATCH 0
#endif

#if defined(__GNUC__)
#define ALWAYS_INLINE inline __attribute ((always_inline))
#else
#define ALWAYS_INLINE inline
#endif

#if defined(_MSC_VER)
#define alloca _alloca
#endif

typedef MIR_val_t *code_t;

typedef struct func_desc {
  MIR_reg_t nregs;
  MIR_item_t func_item;
  MIR_val_t code[1];
} *func_desc_t;

static void update_max_nreg (MIR_reg_t reg, MIR_reg_t *max_nreg) {
  if (*max_nreg < reg) *max_nreg = reg;
}

static MIR_reg_t get_reg (MIR_op_t op, MIR_reg_t *max_nreg) {
  /* We do not interpret code with hard regs */
  mir_assert (op.mode == MIR_OP_REG);
  update_max_nreg (op.u.reg, max_nreg);
  return op.u.reg;
}

#define IC_EL(i) IC_##i
#define REP_SEP ,
typedef enum {
  IC_LDI8 = MIR_INSN_BOUND,
  REP6 (IC_EL, LDU8, LDI16, LDU16, LDI32, LDU32, LDI64),
  REP3 (IC_EL, LDF, LDD, LDLD),
  REP7 (IC_EL, STI8, STU8, STI16, STU16, STI32, STU32, STI64),
  REP8 (IC_EL, STF, STD, STLD, MOVI, MOVP, MOVF, MOVD, MOVLD),
  REP6 (IC_EL, IMM_CALL, IMM_JCALL, MOVFG, FMOVFG, DMOVFG, LDMOVFG),
  REP5 (IC_EL, MOVTG, FMOVTG, DMOVTG, LDMOVTG, INSN_BOUND),
} MIR_full_insn_code_t;
#undef REP_SEP

DEF_VARR (MIR_val_t);

struct ff_interface {
  size_t arg_vars_num, nres, nargs;
  MIR_type_t *res_types;
  _MIR_arg_desc_t *arg_descs;
  void *interface_addr;
};

typedef struct ff_interface *ff_interface_t;
DEF_HTAB (ff_interface_t);

DEF_VARR (_MIR_arg_desc_t);

struct interp_ctx {
#if DIRECT_THREADED_DISPATCH
  void *dispatch_label_tab[IC_INSN_BOUND];
#endif
  MIR_val_t global_regs[MAX_HARD_REG + 1];
  VARR (MIR_val_t) * code_varr;
  VARR (MIR_insn_t) * branches;
  VARR (MIR_val_t) * arg_vals_varr;
  MIR_val_t *arg_vals;
#if MIR_INTERP_TRACE
  int trace_insn_ident;
#endif
  void *(*bstart_builtin) (void);
  void (*bend_builtin) (void *);
  void *jret_addr;
  VARR (MIR_val_t) * call_res_args_varr;
  MIR_val_t *call_res_args;
  VARR (_MIR_arg_desc_t) * call_arg_descs_varr;
  _MIR_arg_desc_t *call_arg_descs;
  HTAB (ff_interface_t) * ff_interface_tab;
};

#define dispatch_label_tab interp_ctx->dispatch_label_tab
#define global_regs interp_ctx->global_regs
#define code_varr interp_ctx->code_varr
#define branches interp_ctx->branches
#define jret_addr interp_ctx->jret_addr
#define arg_vals_varr interp_ctx->arg_vals_varr
#define arg_vals interp_ctx->arg_vals
#define trace_insn_ident interp_ctx->trace_insn_ident
#define trace_ident interp_ctx->trace_ident
#define bstart_builtin interp_ctx->bstart_builtin
#define bend_builtin interp_ctx->bend_builtin
#define call_res_args_varr interp_ctx->call_res_args_varr
#define call_res_args interp_ctx->call_res_args
#define call_arg_descs_varr interp_ctx->call_arg_descs_varr
#define call_arg_descs interp_ctx->call_arg_descs
#define ff_interface_tab interp_ctx->ff_interface_tab

static void get_icode (struct interp_ctx *interp_ctx, MIR_val_t *v, int code) {
#if DIRECT_THREADED_DISPATCH
  v->a = dispatch_label_tab[code];
#else
  v->ic = code;
#endif
}

static void push_insn_start (struct interp_ctx *interp_ctx, int code,
                             MIR_insn_t original_insn MIR_UNUSED) {
  MIR_val_t v;

  get_icode (interp_ctx, &v, code);
  VARR_PUSH (MIR_val_t, code_varr, v);
#if MIR_INTERP_TRACE
  v.a = original_insn;
  VARR_PUSH (MIR_val_t, code_varr, v);
#endif
}

static MIR_full_insn_code_t get_int_mem_insn_code (int load_p, MIR_type_t t) {
  switch (t) {
  case MIR_T_I8: return load_p ? IC_LDI8 : IC_STI8;
  case MIR_T_U8: return load_p ? IC_LDU8 : IC_STU8;
  case MIR_T_I16: return load_p ? IC_LDI16 : IC_STI16;
  case MIR_T_U16: return load_p ? IC_LDU16 : IC_STU16;
  case MIR_T_I32: return load_p ? IC_LDI32 : IC_STI32;
#if MIR_PTR32
  case MIR_T_P:
#endif
  case MIR_T_U32: return load_p ? IC_LDU32 : IC_STU32;
#if MIR_PTR64
  case MIR_T_P:
#endif
  case MIR_T_I64:
  case MIR_T_U64: return load_p ? IC_LDI64 : IC_STI64;
  default: mir_assert (FALSE); return load_p ? IC_LDI64 : IC_STI64; /* to remove a warning */
  }
}

static void push_mem (struct interp_ctx *interp_ctx, MIR_op_t op) {
  MIR_val_t v;

  mir_assert (op.mode == MIR_OP_MEM && op.u.mem.disp == 0 && op.u.mem.index == 0);
  v.i = op.u.mem.base;
  VARR_PUSH (MIR_val_t, code_varr, v);
}

static void redirect_interface_to_interp (MIR_context_t ctx, MIR_item_t func_item);

static void generate_icode (MIR_context_t ctx, MIR_item_t func_item) {
  struct interp_ctx *interp_ctx = ctx->interp_ctx;
  int imm_call_p;
  MIR_func_t func = func_item->u.func;
  MIR_insn_t insn, label;
  MIR_type_t type;
  MIR_val_t v;
  size_t i;
  MIR_reg_t max_nreg = 0;
  func_desc_t func_desc;

  VARR_TRUNC (MIR_insn_t, branches, 0);
  VARR_TRUNC (MIR_val_t, code_varr, 0);
  for (insn = DLIST_HEAD (MIR_insn_t, func->insns); insn != NULL;
       insn = DLIST_NEXT (MIR_insn_t, insn)) {
    MIR_insn_code_t code = insn->code;
    size_t nops = MIR_insn_nops (ctx, insn);
    MIR_op_t *ops = insn->ops;

    insn->data = (void *) VARR_LENGTH (MIR_val_t, code_varr);
    switch (code) {
    case MIR_MOV: /* loads, imm moves */
      if (ops[0].mode == MIR_OP_MEM) {
        push_insn_start (interp_ctx, get_int_mem_insn_code (FALSE, ops[0].u.mem.type), insn);
        v.i = get_reg (ops[1], &max_nreg);
        VARR_PUSH (MIR_val_t, code_varr, v);
        push_mem (interp_ctx, ops[0]);
      } else if (ops[1].mode == MIR_OP_MEM) {
        push_insn_start (interp_ctx, get_int_mem_insn_code (TRUE, ops[1].u.mem.type), insn);
        v.i = get_reg (ops[0], &max_nreg);
        VARR_PUSH (MIR_val_t, code_varr, v);
        push_mem (interp_ctx, ops[1]);
      } else if (ops[1].mode == MIR_OP_INT || ops[1].mode == MIR_OP_UINT) {
        push_insn_start (interp_ctx, IC_MOVI, insn);
        v.i = get_reg (ops[0], &max_nreg);
        VARR_PUSH (MIR_val_t, code_varr, v);
        if (ops[1].mode == MIR_OP_INT)
          v.i = ops[1].u.i;
        else
          v.u = ops[1].u.u;
        VARR_PUSH (MIR_val_t, code_varr, v);
      } else if (ops[1].mode == MIR_OP_REF) {
        MIR_item_t item = ops[1].u.ref;

        if (item->item_type == MIR_import_item && item->ref_def != NULL)
          item->addr = item->ref_def->addr;
        push_insn_start (interp_ctx, IC_MOVP, insn);
        v.i = get_reg (ops[0], &max_nreg);
        VARR_PUSH (MIR_val_t, code_varr, v);
        v.a = item->addr;
        VARR_PUSH (MIR_val_t, code_varr, v);
      } else {
        const char *hard_reg_name;
      regreg:
        mir_assert (ops[0].mode == MIR_OP_REG && ops[1].mode == MIR_OP_REG);
        type = MIR_reg_type (ctx, ops[0].u.reg, func);
        mir_assert (type == MIR_reg_type (ctx, ops[1].u.reg, func));
        if ((hard_reg_name = MIR_reg_hard_reg_name (ctx, ops[0].u.reg, func)) != NULL) {
          mir_assert (MIR_reg_hard_reg_name (ctx, ops[1].u.reg, func) == NULL);
          push_insn_start (interp_ctx,
                           type == MIR_T_F    ? IC_FMOVTG
                           : type == MIR_T_D  ? IC_DMOVTG
                           : type == MIR_T_LD ? IC_LDMOVTG
                                              : IC_MOVTG,
                           insn);
          v.i = _MIR_get_hard_reg (ctx, hard_reg_name);
          mir_assert (v.i <= MAX_HARD_REG);
          VARR_PUSH (MIR_val_t, code_varr, v);
          v.i = get_reg (ops[1], &max_nreg);
          VARR_PUSH (MIR_val_t, code_varr, v);
        } else if ((hard_reg_name = MIR_reg_hard_reg_name (ctx, ops[1].u.reg, func)) != NULL) {
          mir_assert (MIR_reg_hard_reg_name (ctx, ops[0].u.reg, func) == NULL);
          push_insn_start (interp_ctx,
                           type == MIR_T_F    ? IC_FMOVFG
                           : type == MIR_T_D  ? IC_DMOVFG
                           : type == MIR_T_LD ? IC_LDMOVFG
                                              : IC_MOVFG,
                           insn);
          v.i = get_reg (ops[0], &max_nreg);
          VARR_PUSH (MIR_val_t, code_varr, v);
          v.i = _MIR_get_hard_reg (ctx, hard_reg_name);
          mir_assert (v.i <= MAX_HARD_REG);
          VARR_PUSH (MIR_val_t, code_varr, v);
        } else {
          push_insn_start (interp_ctx, code, insn);
          v.i = get_reg (ops[0], &max_nreg);
          VARR_PUSH (MIR_val_t, code_varr, v);
          v.i = get_reg (ops[1], &max_nreg);
          VARR_PUSH (MIR_val_t, code_varr, v);
        }
      }
      break;
    case MIR_FMOV:
      if (ops[0].mode == MIR_OP_MEM) {
        push_insn_start (interp_ctx, IC_STF, insn);
        v.i = get_reg (ops[1], &max_nreg);
        VARR_PUSH (MIR_val_t, code_varr, v);
        push_mem (interp_ctx, ops[0]);
      } else if (ops[1].mode == MIR_OP_MEM) {
        push_insn_start (interp_ctx, IC_LDF, insn);
        v.i = get_reg (ops[0], &max_nreg);
        VARR_PUSH (MIR_val_t, code_varr, v);
        push_mem (interp_ctx, ops[1]);
      } else if (ops[1].mode == MIR_OP_FLOAT) {
        push_insn_start (interp_ctx, IC_MOVF, insn);
        v.i = get_reg (ops[0], &max_nreg);
        VARR_PUSH (MIR_val_t, code_varr, v);
        v.f = ops[1].u.f;
        VARR_PUSH (MIR_val_t, code_varr, v);
      } else {
        goto regreg;
      }
      break;
    case MIR_DMOV:
      if (ops[0].mode == MIR_OP_MEM) {
        push_insn_start (interp_ctx, IC_STD, insn);
        v.i = get_reg (ops[1], &max_nreg);
        VARR_PUSH (MIR_val_t, code_varr, v);
        push_mem (interp_ctx, ops[0]);
      } else if (ops[1].mode == MIR_OP_MEM) {
        push_insn_start (interp_ctx, IC_LDD, insn);
        v.i = get_reg (ops[0], &max_nreg);
        VARR_PUSH (MIR_val_t, code_varr, v);
        push_mem (interp_ctx, ops[1]);
      } else if (ops[1].mode == MIR_OP_DOUBLE) {
        push_insn_start (interp_ctx, IC_MOVD, insn);
        v.i = get_reg (ops[0], &max_nreg);
        VARR_PUSH (MIR_val_t, code_varr, v);
        v.d = ops[1].u.d;
        VARR_PUSH (MIR_val_t, code_varr, v);
      } else {
        goto regreg;
      }
      break;
    case MIR_LDMOV:
      if (ops[0].mode == MIR_OP_MEM) {
        push_insn_start (interp_ctx, IC_STLD, insn);
        v.i = get_reg (ops[1], &max_nreg);
        VARR_PUSH (MIR_val_t, code_varr, v);
        push_mem (interp_ctx, ops[0]);
      } else if (ops[1].mode == MIR_OP_MEM) {
        push_insn_start (interp_ctx, IC_LDLD, insn);
        v.i = get_reg (ops[0], &max_nreg);
        VARR_PUSH (MIR_val_t, code_varr, v);
        push_mem (interp_ctx, ops[1]);
      } else if (ops[1].mode == MIR_OP_LDOUBLE) {
        push_insn_start (interp_ctx, IC_MOVLD, insn);
        v.i = get_reg (ops[0], &max_nreg);
        VARR_PUSH (MIR_val_t, code_varr, v);
        v.ld = ops[1].u.ld;
        VARR_PUSH (MIR_val_t, code_varr, v);
      } else {
        goto regreg;
      }
      break;
    case MIR_LABEL: break;
    case MIR_INVALID_INSN:
      (*MIR_get_error_func (ctx)) (MIR_invalid_insn_error, "invalid insn for interpreter");
      break;
    case MIR_JMP:
      VARR_PUSH (MIR_insn_t, branches, insn);
      push_insn_start (interp_ctx, code, insn);
      v.i = 0;
      VARR_PUSH (MIR_val_t, code_varr, v);
      break;
    case MIR_LADDR:
      VARR_PUSH (MIR_insn_t, branches, insn);
      push_insn_start (interp_ctx, code, insn);
      v.i = get_reg (ops[0], &max_nreg);
      VARR_PUSH (MIR_val_t, code_varr, v);
      v.i = 0;
      VARR_PUSH (MIR_val_t, code_varr, v);
      break;
    case MIR_BT:
    case MIR_BTS:
    case MIR_BF:
    case MIR_BFS:
      VARR_PUSH (MIR_insn_t, branches, insn);
      push_insn_start (interp_ctx, code, insn);
      v.i = 0;
      VARR_PUSH (MIR_val_t, code_varr, v);
      v.i = get_reg (ops[1], &max_nreg);
      VARR_PUSH (MIR_val_t, code_varr, v);
      break;
    case MIR_BEQ:
    case MIR_BEQS:
    case MIR_FBEQ:
    case MIR_DBEQ:
    case MIR_BNE:
    case MIR_BNES:
    case MIR_FBNE:
    case MIR_DBNE:
    case MIR_BLT:
    case MIR_BLTS:
    case MIR_UBLT:
    case MIR_UBLTS:
    case MIR_FBLT:
    case MIR_DBLT:
    case MIR_BLE:
    case MIR_BLES:
    case MIR_UBLE:
    case MIR_UBLES:
    case MIR_FBLE:
    case MIR_DBLE:
    case MIR_BGT:
    case MIR_BGTS:
    case MIR_UBGT:
    case MIR_UBGTS:
    case MIR_FBGT:
    case MIR_DBGT:
    case MIR_BGE:
    case MIR_BGES:
    case MIR_UBGE:
    case MIR_UBGES:
    case MIR_FBGE:
    case MIR_DBGE:
    case MIR_LDBEQ:
    case MIR_LDBNE:
    case MIR_LDBLT:
    case MIR_LDBLE:
    case MIR_LDBGT:
    case MIR_LDBGE:
      VARR_PUSH (MIR_insn_t, branches, insn);
      push_insn_start (interp_ctx, code, insn);
      v.i = 0;
      VARR_PUSH (MIR_val_t, code_varr, v);
      v.i = get_reg (ops[1], &max_nreg);
      VARR_PUSH (MIR_val_t, code_varr, v);
      v.i = get_reg (ops[2], &max_nreg);
      VARR_PUSH (MIR_val_t, code_varr, v);
      break;
    case MIR_BO:
    case MIR_UBO:
    case MIR_BNO:
    case MIR_UBNO:
      VARR_PUSH (MIR_insn_t, branches, insn);
      push_insn_start (interp_ctx, code, insn);
      v.i = 0;
      VARR_PUSH (MIR_val_t, code_varr, v);
      break;
    case MIR_PRSET: break; /* just ignore */
    case MIR_PRBEQ:        /* make jump if property is zero or ignore otherwise */
      if (ops[2].mode == MIR_OP_INT && ops[2].u.i == 0) goto jump;
      break;
    case MIR_PRBNE: /* make jump if property is nonzero or ignore otherwise */
      if (ops[2].mode != MIR_OP_INT || ops[2].u.i == 0) break;
    jump:
      VARR_PUSH (MIR_insn_t, branches, insn);
      push_insn_start (interp_ctx, MIR_JMP, insn);
      v.i = 0;
      VARR_PUSH (MIR_val_t, code_varr, v); /* place for label */
      break;
    default:
      imm_call_p = FALSE;
      if (MIR_call_code_p (code))
        imm_call_p = (ops[1].mode == MIR_OP_REF
                      && (ops[1].u.ref->item_type == MIR_import_item
                          || ops[1].u.ref->item_type == MIR_export_item
                          || ops[1].u.ref->item_type == MIR_forward_item
                          || ops[1].u.ref->item_type == MIR_func_item));
      push_insn_start (interp_ctx,
                       imm_call_p           ? (code == MIR_JCALL ? IC_IMM_JCALL : IC_IMM_CALL)
                       : code == MIR_INLINE ? MIR_CALL
                                            : code,
                       insn);
      if (code == MIR_SWITCH) {
        VARR_PUSH (MIR_insn_t, branches, insn);
        v.i = nops;
        VARR_PUSH (MIR_val_t, code_varr, v);
      } else if (code == MIR_RET) {
        v.i = nops;
        VARR_PUSH (MIR_val_t, code_varr, v);
      } else if (MIR_call_code_p (code)) {
        v.i = nops;
        VARR_PUSH (MIR_val_t, code_varr, v);
        v.a = insn;
        VARR_PUSH (MIR_val_t, code_varr, v);
        v.a = NULL;
        VARR_PUSH (MIR_val_t, code_varr, v); /* for ffi interface */
      }
      for (i = 0; i < nops; i++) {
        if (i == 0 && MIR_call_code_p (code)) { /* prototype ??? */
          mir_assert (ops[i].mode == MIR_OP_REF && ops[i].u.ref->item_type == MIR_proto_item);
          v.a = ops[i].u.ref;
        } else if (i == 1 && imm_call_p) {
          MIR_item_t item = ops[i].u.ref;

          mir_assert (item->item_type == MIR_import_item || item->item_type == MIR_export_item
                      || item->item_type == MIR_forward_item || item->item_type == MIR_func_item);
          v.a = item->addr;
        } else if (code == MIR_VA_ARG && i == 2) { /* type */
          mir_assert (ops[i].mode == MIR_OP_MEM);
          v.i = ops[i].u.mem.type;
        } else if (code == MIR_SWITCH && i > 0) {
          mir_assert (ops[i].mode == MIR_OP_LABEL);
          v.i = 0;
        } else if (MIR_call_code_p (code) && ops[i].mode == MIR_OP_MEM) {
          mir_assert (MIR_all_blk_type_p (ops[i].u.mem.type));
          v.i = ops[i].u.mem.base;
          update_max_nreg ((MIR_reg_t) v.i, &max_nreg);
        } else {
          mir_assert (ops[i].mode == MIR_OP_REG);
          v.i = get_reg (ops[i], &max_nreg);
        }
        VARR_PUSH (MIR_val_t, code_varr, v);
      }
    }
  }
  for (i = 0; i < VARR_LENGTH (MIR_insn_t, branches); i++) {
    size_t start_label_nop = 0, bound_label_nop = 1, start_label_loc = 1, n;

    insn = VARR_GET (MIR_insn_t, branches, i);
    if (insn->code == MIR_LADDR) {
      start_label_nop = 1;
      bound_label_nop = 2;
    } else if (insn->code == MIR_SWITCH) {
      start_label_nop = 1;
      bound_label_nop = start_label_nop + insn->nops - 1;
      start_label_loc++; /* we put nops for MIR_SWITCH */
    }
    for (n = start_label_nop; n < bound_label_nop; n++) {
      label = insn->ops[n].u.label;
      v.i = (size_t) label->data;
#if MIR_INTERP_TRACE
      VARR_SET (MIR_val_t, code_varr, (size_t) insn->data + n + start_label_loc + 1, v);
#else
      VARR_SET (MIR_val_t, code_varr, (size_t) insn->data + n + start_label_loc, v);
#endif
    }
  }
  func_item->data = func_desc
    = MIR_malloc (ctx->alloc, sizeof (struct func_desc) + VARR_LENGTH (MIR_val_t, code_varr) * sizeof (MIR_val_t));
  if (func_desc == NULL)
    (*MIR_get_error_func (ctx)) (MIR_alloc_error, "no memory for interpreter code");
  memmove (func_desc->code, VARR_ADDR (MIR_val_t, code_varr),
           VARR_LENGTH (MIR_val_t, code_varr) * sizeof (MIR_val_t));
  for (MIR_lref_data_t lref = func->first_lref; lref != NULL; lref = lref->next) {
    if (lref->label2 == NULL)
      *(void **) lref->load_addr
        = (char *) (func_desc->code + (int64_t) lref->label->data) + lref->disp;
    else
      *(int64_t *) lref->load_addr
        = (int64_t) lref->label->data - (int64_t) lref->label2->data + lref->disp;
  }
  mir_assert (max_nreg < MIR_MAX_REG_NUM);
  func_desc->nregs = max_nreg + 1;
  func_desc->func_item = func_item;
}

static void finish_func_interpretation (MIR_item_t func_item, MIR_alloc_t alloc) {
  mir_assert (func_item->item_type == MIR_func_item);
  if (func_item->data == NULL) return;
  for (MIR_insn_t insn = DLIST_HEAD (MIR_insn_t, func_item->u.func->insns); insn != NULL;
       insn = DLIST_NEXT (MIR_insn_t, insn))
    insn->data = NULL; /* it was used for interpretation preparation */
  MIR_free (alloc, func_item->data);
  func_item->data = NULL;
}

static ALWAYS_INLINE void *get_a (MIR_val_t *v) { return v->a; }
static ALWAYS_INLINE int64_t get_i (MIR_val_t *v) { return v->i; }
static ALWAYS_INLINE float get_f (MIR_val_t *v) { return v->f; }
static ALWAYS_INLINE double get_d (MIR_val_t *v) { return v->d; }
static ALWAYS_INLINE long double get_ld (MIR_val_t *v) { return v->ld; }

static ALWAYS_INLINE void **get_aop (MIR_val_t *bp, code_t c) { return &bp[get_i (c)].a; }
static ALWAYS_INLINE int64_t *get_iop (MIR_val_t *bp, code_t c) { return &bp[get_i (c)].i; }
static ALWAYS_INLINE uint64_t *get_uop (MIR_val_t *bp, code_t c) { return &bp[get_i (c)].u; }
static ALWAYS_INLINE float *get_fop (MIR_val_t *bp, code_t c) { return &bp[get_i (c)].f; }
static ALWAYS_INLINE double *get_dop (MIR_val_t *bp, code_t c) { return &bp[get_i (c)].d; }
static ALWAYS_INLINE long double *get_ldop (MIR_val_t *bp, code_t c) { return &bp[get_i (c)].ld; }

static ALWAYS_INLINE int64_t *get_2iops (MIR_val_t *bp, code_t c, int64_t *p) {
  *p = *get_iop (bp, c + 1);
  return get_iop (bp, c);
}

static ALWAYS_INLINE int64_t *get_2isops (MIR_val_t *bp, code_t c, int32_t *p) {
  *p = (int32_t) *get_iop (bp, c + 1);
  return get_iop (bp, c);
}

static ALWAYS_INLINE int64_t *get_3iops (MIR_val_t *bp, code_t c, int64_t *p1, int64_t *p2) {
  *p1 = *get_iop (bp, c + 1);
  *p2 = *get_iop (bp, c + 2);
  return get_iop (bp, c);
}

static ALWAYS_INLINE int64_t *get_3isops (MIR_val_t *bp, code_t c, int32_t *p1, int32_t *p2) {
  *p1 = (int32_t) *get_iop (bp, c + 1);
  *p2 = (int32_t) *get_iop (bp, c + 2);
  return get_iop (bp, c);
}

static ALWAYS_INLINE uint64_t *get_3uops (MIR_val_t *bp, code_t c, uint64_t *p1, uint64_t *p2) {
  *p1 = *get_uop (bp, c + 1);
  *p2 = *get_uop (bp, c + 2);
  return get_uop (bp, c);
}

static ALWAYS_INLINE uint64_t *get_3usops (MIR_val_t *bp, code_t c, uint32_t *p1, uint32_t *p2) {
  *p1 = (uint32_t) *get_uop (bp, c + 1);
  *p2 = (uint32_t) *get_uop (bp, c + 2);
  return get_uop (bp, c);
}

static ALWAYS_INLINE float *get_2fops (MIR_val_t *bp, code_t c, float *p) {
  *p = *get_fop (bp, c + 1);
  return get_fop (bp, c);
}

static ALWAYS_INLINE float *get_3fops (MIR_val_t *bp, code_t c, float *p1, float *p2) {
  *p1 = *get_fop (bp, c + 1);
  *p2 = *get_fop (bp, c + 2);
  return get_fop (bp, c);
}

static ALWAYS_INLINE int64_t *get_fcmp_ops (MIR_val_t *bp, code_t c, float *p1, float *p2) {
  *p1 = *get_fop (bp, c + 1);
  *p2 = *get_fop (bp, c + 2);
  return get_iop (bp, c);
}

static ALWAYS_INLINE double *get_2dops (MIR_val_t *bp, code_t c, double *p) {
  *p = *get_dop (bp, c + 1);
  return get_dop (bp, c);
}

static ALWAYS_INLINE double *get_3dops (MIR_val_t *bp, code_t c, double *p1, double *p2) {
  *p1 = *get_dop (bp, c + 1);
  *p2 = *get_dop (bp, c + 2);
  return get_dop (bp, c);
}

static ALWAYS_INLINE int64_t *get_dcmp_ops (MIR_val_t *bp, code_t c, double *p1, double *p2) {
  *p1 = *get_dop (bp, c + 1);
  *p2 = *get_dop (bp, c + 2);
  return get_iop (bp, c);
}

static ALWAYS_INLINE long double *get_2ldops (MIR_val_t *bp, code_t c, long double *p) {
  *p = *get_ldop (bp, c + 1);
  return get_ldop (bp, c);
}

static ALWAYS_INLINE long double *get_3ldops (MIR_val_t *bp, code_t c, long double *p1,
                                              long double *p2) {
  *p1 = *get_ldop (bp, c + 1);
  *p2 = *get_ldop (bp, c + 2);
  return get_ldop (bp, c);
}

static ALWAYS_INLINE int64_t *get_ldcmp_ops (MIR_val_t *bp, code_t c, long double *p1,
                                             long double *p2) {
  *p1 = *get_ldop (bp, c + 1);
  *p2 = *get_ldop (bp, c + 2);
  return get_iop (bp, c);
}

static ALWAYS_INLINE int64_t get_mem_addr (MIR_val_t *bp, code_t c) { return bp[get_i (c)].i; }

#define EXT(tp)                          \
  do {                                   \
    int64_t *r = get_iop (bp, ops);      \
    tp s = (tp) * get_iop (bp, ops + 1); \
    *r = (int64_t) s;                    \
  } while (0)
#define IOP2(op)                 \
  do {                           \
    int64_t *r, p;               \
    r = get_2iops (bp, ops, &p); \
    *r = op p;                   \
  } while (0)
#define IOP2S(op)                 \
  do {                            \
    int64_t *r;                   \
    int32_t p;                    \
    r = get_2isops (bp, ops, &p); \
    *r = op p;                    \
  } while (0)
#define IOP3(op)                       \
  do {                                 \
    int64_t *r, p1, p2;                \
    r = get_3iops (bp, ops, &p1, &p2); \
    *r = p1 op p2;                     \
  } while (0)
#define IOP3S(op)                       \
  do {                                  \
    int64_t *r;                         \
    int32_t p1, p2;                     \
    r = get_3isops (bp, ops, &p1, &p2); \
    *r = p1 op p2;                      \
  } while (0)
#define ICMP(op)                       \
  do {                                 \
    int64_t *r, p1, p2;                \
    r = get_3iops (bp, ops, &p1, &p2); \
    *r = p1 op p2;                     \
  } while (0)
#define ICMPS(op)                       \
  do {                                  \
    int64_t *r;                         \
    int32_t p1, p2;                     \
    r = get_3isops (bp, ops, &p1, &p2); \
    *r = p1 op p2;                      \
  } while (0)
#define BICMP(op)                                                       \
  do {                                                                  \
    int64_t op1 = *get_iop (bp, ops + 1), op2 = *get_iop (bp, ops + 2); \
    if (op1 op op2) pc = code + get_i (ops);                            \
  } while (0)
#define BICMPS(op)                                                                            \
  do {                                                                                        \
    int32_t op1 = (int32_t) * get_iop (bp, ops + 1), op2 = (int32_t) * get_iop (bp, ops + 2); \
    if (op1 op op2) pc = code + get_i (ops);                                                  \
  } while (0)
#define UOP3(op)                       \
  do {                                 \
    uint64_t *r, p1, p2;               \
    r = get_3uops (bp, ops, &p1, &p2); \
    *r = p1 op p2;                     \
  } while (0)
#define UOP3S(op)                       \
  do {                                  \
    uint64_t *r;                        \
    uint32_t p1, p2;                    \
    r = get_3usops (bp, ops, &p1, &p2); \
    *r = p1 op p2;                      \
  } while (0)
#define UIOP3(op)                      \
  do {                                 \
    uint64_t *r, p1, p2;               \
    r = get_3uops (bp, ops, &p1, &p2); \
    *r = p1 op p2;                     \
  } while (0)
#define UIOP3S(op)                      \
  do {                                  \
    uint64_t *r;                        \
    uint32_t p1, p2;                    \
    r = get_3usops (bp, ops, &p1, &p2); \
    *r = p1 op p2;                      \
  } while (0)
#define UCMP(op)                       \
  do {                                 \
    uint64_t *r, p1, p2;               \
    r = get_3uops (bp, ops, &p1, &p2); \
    *r = p1 op p2;                     \
  } while (0)
#define UCMPS(op)                       \
  do {                                  \
    uint64_t *r;                        \
    uint32_t p1, p2;                    \
    r = get_3usops (bp, ops, &p1, &p2); \
    *r = p1 op p2;                      \
  } while (0)
#define BUCMP(op)                                                        \
  do {                                                                   \
    uint64_t op1 = *get_uop (bp, ops + 1), op2 = *get_uop (bp, ops + 2); \
    if (op1 op op2) pc = code + get_i (ops);                             \
  } while (0)
#define BUCMPS(op)                                                                               \
  do {                                                                                           \
    uint32_t op1 = (uint32_t) * get_uop (bp, ops + 1), op2 = (uint32_t) * get_uop (bp, ops + 2); \
    if (op1 op op2) pc = code + get_i (ops);                                                     \
  } while (0)

#define FOP2(op)                 \
  do {                           \
    float *r, p;                 \
    r = get_2fops (bp, ops, &p); \
    *r = op p;                   \
  } while (0)
#define FOP3(op)                       \
  do {                                 \
    float *r, p1, p2;                  \
    r = get_3fops (bp, ops, &p1, &p2); \
    *r = p1 op p2;                     \
  } while (0)
#define FCMP(op)                          \
  do {                                    \
    int64_t *r;                           \
    float p1, p2;                         \
    r = get_fcmp_ops (bp, ops, &p1, &p2); \
    *r = p1 op p2;                        \
  } while (0)
#define BFCMP(op)                                                     \
  do {                                                                \
    float op1 = *get_fop (bp, ops + 1), op2 = *get_fop (bp, ops + 2); \
    if (op1 op op2) pc = code + get_i (ops);                          \
  } while (0)

#define DOP2(op)                 \
  do {                           \
    double *r, p;                \
    r = get_2dops (bp, ops, &p); \
    *r = op p;                   \
  } while (0)
#define DOP3(op)                       \
  do {                                 \
    double *r, p1, p2;                 \
    r = get_3dops (bp, ops, &p1, &p2); \
    *r = p1 op p2;                     \
  } while (0)
#define DCMP(op)                          \
  do {                                    \
    int64_t *r;                           \
    double p1, p2;                        \
    r = get_dcmp_ops (bp, ops, &p1, &p2); \
    *r = p1 op p2;                        \
  } while (0)
#define BDCMP(op)                                                      \
  do {                                                                 \
    double op1 = *get_dop (bp, ops + 1), op2 = *get_dop (bp, ops + 2); \
    if (op1 op op2) pc = code + get_i (ops);                           \
  } while (0)

#define LDOP2(op)                 \
  do {                            \
    long double *r, p;            \
    r = get_2ldops (bp, ops, &p); \
    *r = op p;                    \
  } while (0)
#define LDOP3(op)                       \
  do {                                  \
    long double *r, p1, p2;             \
    r = get_3ldops (bp, ops, &p1, &p2); \
    *r = p1 op p2;                      \
  } while (0)
#define LDCMP(op)                          \
  do {                                     \
    int64_t *r;                            \
    long double p1, p2;                    \
    r = get_ldcmp_ops (bp, ops, &p1, &p2); \
    *r = p1 op p2;                         \
  } while (0)
#define BLDCMP(op)                                                            \
  do {                                                                        \
    long double op1 = *get_ldop (bp, ops + 1), op2 = *get_ldop (bp, ops + 2); \
    if (op1 op op2) pc = code + get_i (ops);                                  \
  } while (0)

#define LD(op, val_type, mem_type)          \
  do {                                      \
    val_type *r = get_##op (bp, ops);       \
    int64_t a = get_mem_addr (bp, ops + 1); \
    *r = *((mem_type *) a);                 \
  } while (0)
#define ST(op, val_type, mem_type)                \
  do {                                            \
    val_type v = (val_type) * get_##op (bp, ops); \
    int64_t a = get_mem_addr (bp, ops + 1);       \
    *((mem_type *) a) = (mem_type) v;             \
  } while (0)

#if !MIR_INTERP_TRACE && defined(__GNUC__) && !defined(__clang__)
#define OPTIMIZE \
  __attribute__ ((__optimize__ ("O2"))) __attribute__ ((__optimize__ ("-fno-ipa-cp-clone")))
#else
#define OPTIMIZE
#endif

static void call (MIR_context_t ctx, MIR_val_t *bp, MIR_op_t *insn_arg_ops, code_t ffi_address_ptr,
                  MIR_item_t proto_item, void *addr, code_t res_ops, size_t nargs);

#if MIR_INTERP_TRACE
static void start_insn_trace (MIR_context_t ctx, const char *name, func_desc_t func_desc, code_t pc,
                              size_t nops) {
  struct interp_ctx *interp_ctx = ctx->interp_ctx;
  MIR_insn_t insn = pc[1].a;
  code_t ops = pc + 2;

  for (int i = 0; i < trace_insn_ident; i++) fprintf (stderr, " ");
  fprintf (stderr, "%s", name);
  for (size_t i = 0; i < nops; i++) {
    fprintf (stderr, i == 0 ? "\t" : ", ");
    fprintf (stderr, "%" PRId64, ops[i].i);
  }
  fprintf (stderr, "\t#");
  MIR_output_insn (ctx, stderr, insn, func_desc->func_item->u.func, FALSE);
}

static void finish_insn_trace (MIR_context_t ctx, MIR_full_insn_code_t code, code_t ops,
                               MIR_val_t *bp) {
  struct interp_ctx *interp_ctx = ctx->interp_ctx;
  int out_p;
  MIR_op_mode_t op_mode = MIR_OP_UNDEF;
  MIR_val_t *res = bp;

  switch (code) {
  case IC_LDI8:
  case IC_LDU8:
  case IC_LDI16:
  case IC_LDU16:
  case IC_LDI32:
  case IC_LDU32:
  case IC_LDI64:
  case IC_MOVI:
  case IC_MOVTG:
    res = global_regs;
    /* falls through */
  case IC_MOVFG:
  case IC_MOVP: op_mode = MIR_OP_INT; break;
  case IC_LDF:
  case IC_FMOVTG:
    res = global_regs;
    /* falls through */
  case IC_FMOVFG:
  case IC_MOVF: op_mode = MIR_OP_FLOAT; break;
  case IC_LDD:
  case IC_DMOVTG:
    res = global_regs;
    /* falls through */
  case IC_DMOVFG:
  case IC_MOVD: op_mode = MIR_OP_DOUBLE; break;
  case IC_LDLD:
  case IC_LDMOVTG:
    res = global_regs;
    /* falls through */
  case IC_LDMOVFG:
  case IC_MOVLD: op_mode = MIR_OP_LDOUBLE; break;
  case IC_STI8:
  case IC_STU8:
  case IC_STI16:
  case IC_STU16:
  case IC_STI32:
  case IC_STU32:
  case IC_STI64:
  case IC_STF:
  case IC_STD:;
  case IC_STLD: break;
  case IC_IMM_CALL: break;
  case IC_IMM_JCALL: break;
  default:
    op_mode = _MIR_insn_code_op_mode (ctx, (MIR_insn_code_t) code, 0, &out_p);
    if (op_mode == MIR_OP_BOUND || !out_p) op_mode = MIR_OP_UNDEF;
    break;
  }
  switch (op_mode) {
  case MIR_OP_INT:
  case MIR_OP_UINT:
    fprintf (stderr, "\t# res = %" PRId64 " (%" PRIu64 "u, 0x%" PRIx64 ")", res[ops[0].i].i,
             res[ops[0].i].u, res[ops[0].i].u);
    break;
  case MIR_OP_FLOAT: fprintf (stderr, "\t# res = %.*ef", FLT_DECIMAL_DIG, res[ops[0].i].f); break;
  case MIR_OP_LDOUBLE:
#ifndef _WIN32
    fprintf (stderr, "\t# res = %.*Le", LDBL_DECIMAL_DIG, res[ops[0].i].ld);
    break;
#endif
  case MIR_OP_DOUBLE: fprintf (stderr, "\t# res = %.*e", DBL_DECIMAL_DIG, res[ops[0].i].d); break;
  default: assert (op_mode == MIR_OP_UNDEF);
  }
  fprintf (stderr, "\n");
}
#endif

static code_t call_insn_execute (MIR_context_t ctx, code_t pc, MIR_val_t *bp, code_t ops,
                                 int imm_p) {
  struct interp_ctx *interp_ctx = ctx->interp_ctx;
  int64_t nops = get_i (ops); /* #args w/o nop, insn, and ff interface address */
  MIR_insn_t insn = get_a (ops + 1);
  MIR_item_t proto_item = get_a (ops + 3);
  void *func_addr = imm_p ? get_a (ops + 4) : *get_aop (bp, ops + 4);
  size_t start = proto_item->u.proto->nres + 5;

  if (VARR_EXPAND (MIR_val_t, arg_vals_varr, nops)) arg_vals = VARR_ADDR (MIR_val_t, arg_vals_varr);

  for (size_t i = start; i < (size_t) nops + 3; i++) arg_vals[i - start] = bp[get_i (ops + i)];

#if MIR_INTERP_TRACE
  trace_insn_ident += 2;
#endif
  call (ctx, bp, &insn->ops[proto_item->u.proto->nres + 2] /* arg ops */,
        ops + 2 /* ffi address holder */, proto_item, func_addr, ops + 5 /* results start */,
        nops - start + 3 /* arg # */);
#if MIR_INTERP_TRACE
  trace_insn_ident -= 2;
#endif
  pc += nops + 3; /* nops itself, the call insn, add ff interface address */
  return pc;
}

static int64_t addr_offset8, addr_offset16, addr_offset32;

static void OPTIMIZE eval (MIR_context_t ctx, func_desc_t func_desc, MIR_val_t *bp,
                           MIR_val_t *results) {
  struct interp_ctx *interp_ctx = ctx->interp_ctx;
  MIR_val_t *globals = global_regs;
  code_t pc, ops, code;
  void *jmpi_val; /* where label thunk execution result will be: */
  int64_t offset;
  int signed_overflow_p = FALSE, unsigned_overflow_p = FALSE; /* to avoid uninitialized warnings */

#if MIR_INTERP_TRACE
  MIR_full_insn_code_t trace_insn_code;
#define START_INSN(v, nops)                          \
  do {                                               \
    trace_insn_code = (MIR_full_insn_code_t) v;      \
    start_insn_trace (ctx, #v, func_desc, pc, nops); \
    ops = pc + 2; /* skip original insn too */       \
    pc += nops + 2;                                  \
  } while (0)
#else
#define START_INSN(v, nops) \
  do {                      \
    ops = pc + 1;           \
    pc += nops + 1;         \
  } while (0)
#endif

#if DIRECT_THREADED_DISPATCH
  void **ltab = dispatch_label_tab;

#define LAB_EL(i) ltab[i] = &&L_##i
#define REP_SEP ;
  if (bp == NULL) {
    REP4 (LAB_EL, MIR_MOV, MIR_FMOV, MIR_DMOV, MIR_LDMOV);
    REP6 (LAB_EL, MIR_EXT8, MIR_EXT16, MIR_EXT32, MIR_UEXT8, MIR_UEXT16, MIR_UEXT32);
    REP6 (LAB_EL, MIR_I2F, MIR_I2D, MIR_I2LD, MIR_UI2F, MIR_UI2D, MIR_UI2LD);
    REP8 (LAB_EL, MIR_F2I, MIR_D2I, MIR_LD2I, MIR_F2D, MIR_F2LD, MIR_D2F, MIR_D2LD, MIR_LD2F);
    REP6 (LAB_EL, MIR_LD2D, MIR_NEG, MIR_NEGS, MIR_FNEG, MIR_DNEG, MIR_LDNEG);
    REP6 (LAB_EL, MIR_ADDR, MIR_ADDR8, MIR_ADDR16, MIR_ADDR32, MIR_ADD, MIR_ADDS);
    REP8 (LAB_EL, MIR_FADD, MIR_DADD, MIR_LDADD, MIR_SUB, MIR_SUBS, MIR_FSUB, MIR_DSUB, MIR_LDSUB);
    REP8 (LAB_EL, MIR_MUL, MIR_MULS, MIR_FMUL, MIR_DMUL, MIR_LDMUL, MIR_DIV, MIR_DIVS, MIR_UDIV);
    REP8 (LAB_EL, MIR_UDIVS, MIR_FDIV, MIR_DDIV, MIR_LDDIV, MIR_MOD, MIR_MODS, MIR_UMOD, MIR_UMODS);
    REP8 (LAB_EL, MIR_AND, MIR_ANDS, MIR_OR, MIR_ORS, MIR_XOR, MIR_XORS, MIR_LSH, MIR_LSHS);
    REP8 (LAB_EL, MIR_RSH, MIR_RSHS, MIR_URSH, MIR_URSHS, MIR_EQ, MIR_EQS, MIR_FEQ, MIR_DEQ);
    REP8 (LAB_EL, MIR_LDEQ, MIR_NE, MIR_NES, MIR_FNE, MIR_DNE, MIR_LDNE, MIR_LT, MIR_LTS);
    REP8 (LAB_EL, MIR_ULT, MIR_ULTS, MIR_FLT, MIR_DLT, MIR_LDLT, MIR_LE, MIR_LES, MIR_ULE);
    REP8 (LAB_EL, MIR_ULES, MIR_FLE, MIR_DLE, MIR_LDLE, MIR_GT, MIR_GTS, MIR_UGT, MIR_UGTS);
    REP8 (LAB_EL, MIR_FGT, MIR_DGT, MIR_LDGT, MIR_GE, MIR_GES, MIR_UGE, MIR_UGES, MIR_FGE);
    REP6 (LAB_EL, MIR_DGE, MIR_LDGE, MIR_ADDO, MIR_ADDOS, MIR_SUBO, MIR_SUBOS);
    REP4 (LAB_EL, MIR_MULO, MIR_MULOS, MIR_UMULO, MIR_UMULOS);
    REP6 (LAB_EL, MIR_JMP, MIR_BT, MIR_BTS, MIR_BF, MIR_BFS, MIR_BEQ);
    REP8 (LAB_EL, MIR_BEQS, MIR_FBEQ, MIR_DBEQ, MIR_LDBEQ, MIR_BNE, MIR_BNES, MIR_FBNE, MIR_DBNE);
    REP8 (LAB_EL, MIR_LDBNE, MIR_BLT, MIR_BLTS, MIR_UBLT, MIR_UBLTS, MIR_FBLT, MIR_DBLT, MIR_LDBLT);
    REP8 (LAB_EL, MIR_BLE, MIR_BLES, MIR_UBLE, MIR_UBLES, MIR_FBLE, MIR_DBLE, MIR_LDBLE, MIR_BGT);
    REP8 (LAB_EL, MIR_BGTS, MIR_UBGT, MIR_UBGTS, MIR_FBGT, MIR_DBGT, MIR_LDBGT, MIR_BGE, MIR_BGES);
    REP5 (LAB_EL, MIR_UBGE, MIR_UBGES, MIR_FBGE, MIR_DBGE, MIR_LDBGE);
    REP6 (LAB_EL, MIR_BO, MIR_UBO, MIR_BNO, MIR_UBNO, MIR_LADDR, MIR_JMPI);
    REP6 (LAB_EL, MIR_CALL, MIR_INLINE, MIR_JCALL, MIR_SWITCH, MIR_RET, MIR_JRET);
    REP3 (LAB_EL, MIR_ALLOCA, MIR_BSTART, MIR_BEND);
    REP4 (LAB_EL, MIR_VA_ARG, MIR_VA_BLOCK_ARG, MIR_VA_START, MIR_VA_END);
    REP8 (LAB_EL, IC_LDI8, IC_LDU8, IC_LDI16, IC_LDU16, IC_LDI32, IC_LDU32, IC_LDI64, IC_LDF);
    REP8 (LAB_EL, IC_LDD, IC_LDLD, IC_STI8, IC_STU8, IC_STI16, IC_STU16, IC_STI32, IC_STU32);
    REP8 (LAB_EL, IC_STI64, IC_STF, IC_STD, IC_STLD, IC_MOVI, IC_MOVP, IC_MOVF, IC_MOVD);
    REP3 (LAB_EL, IC_MOVLD, IC_IMM_CALL, IC_IMM_JCALL);
    REP4 (LAB_EL, IC_MOVFG, IC_FMOVFG, IC_DMOVFG, IC_LDMOVFG);
    REP4 (LAB_EL, IC_MOVTG, IC_FMOVTG, IC_DMOVTG, IC_LDMOVTG);
    return;
  }
#undef REP_SEP
#define CASE0(value) L_##value:

#if MIR_INTERP_TRACE
#define END_INSN                                     \
  finish_insn_trace (ctx, trace_insn_code, ops, bp); \
  goto * pc->a
#else
#define END_INSN goto * pc->a
#endif

#else

#define CASE0(value) case value:

#if MIR_INTERP_TRACE
#define END_INSN                                     \
  finish_insn_trace (ctx, trace_insn_code, ops, bp); \
  break
#else
#define END_INSN break
#endif

#endif

#define CASE(value, nops) CASE0 (value) START_INSN (value, nops);

#define SCASE(insn, nop, stmt) \
  CASE (insn, nop) {           \
    stmt;                      \
    END_INSN;                  \
  }

  code = func_desc->code;
  pc = code;

#if DIRECT_THREADED_DISPATCH
  goto * pc->a;
#else
  for (;;) {
    int insn_code = pc->ic;
    switch (insn_code) {
#endif

#if 0
    L_jmpi_finish :
#endif
  { /* jmpi thunk return */
    pc = jmpi_val;
    END_INSN;
  }

  CASE (MIR_MOV, 2) {
    int64_t p, *r = get_2iops (bp, ops, &p);
    *r = p;
    END_INSN;
  }
  CASE (MIR_FMOV, 2) {
    float p, *r = get_2fops (bp, ops, &p);
    *r = p;
    END_INSN;
  }
  CASE (MIR_DMOV, 2) {
    double p, *r = get_2dops (bp, ops, &p);
    *r = p;
    END_INSN;
  }
  CASE (MIR_LDMOV, 2) {
    long double p, *r = get_2ldops (bp, ops, &p);
    *r = p;
    END_INSN;
  }

  CASE (IC_MOVFG, 2) {
    int64_t l = get_i (ops), r = get_i (ops + 1);
    bp[l].i = globals[r].i;
    END_INSN;
  }
  CASE (IC_FMOVFG, 2) {
    int64_t l = get_i (ops), r = get_i (ops + 1);
    bp[l].f = globals[r].f;
    END_INSN;
  }
  CASE (IC_DMOVFG, 2) {
    int64_t l = get_i (ops), r = get_i (ops + 1);
    bp[l].d = globals[r].d;
    END_INSN;
  }
  CASE (IC_LDMOVFG, 2) {
    int64_t l = get_i (ops), r = get_i (ops + 1);
    bp[l].ld = globals[r].ld;
    END_INSN;
  }

  CASE (IC_MOVTG, 2) {
    int64_t l = get_i (ops), r = get_i (ops + 1);
    globals[l].i = bp[r].i;
    END_INSN;
  }
  CASE (IC_FMOVTG, 2) {
    int64_t l = get_i (ops), r = get_i (ops + 1);
    globals[l].f = bp[r].f;
    END_INSN;
  }
  CASE (IC_DMOVTG, 2) {
    int64_t l = get_i (ops), r = get_i (ops + 1);
    globals[l].d = bp[r].d;
    END_INSN;
  }
  CASE (IC_LDMOVTG, 2) {
    int64_t l = get_i (ops), r = get_i (ops + 1);
    globals[l].ld = bp[r].ld;
    END_INSN;
  }

  SCASE (MIR_EXT8, 2, EXT (int8_t));
  SCASE (MIR_EXT16, 2, EXT (int16_t));
  SCASE (MIR_EXT32, 2, EXT (int32_t));
  SCASE (MIR_UEXT8, 2, EXT (uint8_t));
  SCASE (MIR_UEXT16, 2, EXT (uint16_t));
  SCASE (MIR_UEXT32, 2, EXT (uint32_t));
  CASE (MIR_I2F, 2) {
    float *r = get_fop (bp, ops);
    int64_t i = *get_iop (bp, ops + 1);

    *r = (float) i;
    END_INSN;
  }
  CASE (MIR_I2D, 2) {
    double *r = get_dop (bp, ops);
    int64_t i = *get_iop (bp, ops + 1);

    *r = (double) i;
    END_INSN;
  }
  CASE (MIR_I2LD, 2) {
    long double *r = get_ldop (bp, ops);
    int64_t i = *get_iop (bp, ops + 1);

    *r = (long double) i;
    END_INSN;
  }

  CASE (MIR_UI2F, 2) {
    float *r = get_fop (bp, ops);
    uint64_t i = *get_iop (bp, ops + 1);

    *r = (float) i;
    END_INSN;
  }
  CASE (MIR_UI2D, 2) {
    double *r = get_dop (bp, ops);
    uint64_t i = *get_iop (bp, ops + 1);

    *r = (double) i;
    END_INSN;
  }
  CASE (MIR_UI2LD, 2) {
    long double *r = get_ldop (bp, ops);
    uint64_t i = *get_iop (bp, ops + 1);

    *r = (long double) i;
    END_INSN;
  }

  CASE (MIR_F2I, 2) {
    int64_t *r = get_iop (bp, ops);
    float f = *get_fop (bp, ops + 1);

    *r = (int64_t) f;
    END_INSN;
  }
  CASE (MIR_D2I, 2) {
    int64_t *r = get_iop (bp, ops);
    double d = *get_dop (bp, ops + 1);

    *r = (int64_t) d;
    END_INSN;
  }
  CASE (MIR_LD2I, 2) {
    int64_t *r = get_iop (bp, ops);
    long double ld = *get_ldop (bp, ops + 1);

    *r = (int64_t) ld;
    END_INSN;
  }

  CASE (MIR_F2D, 2) {
    double *r = get_dop (bp, ops);
    float f = *get_fop (bp, ops + 1);
    *r = f;
    END_INSN;
  }
  CASE (MIR_F2LD, 2) {
    long double *r = get_ldop (bp, ops);
    float f = *get_fop (bp, ops + 1);

    *r = f;
    END_INSN;
  }
  CASE (MIR_D2F, 2) {
    float *r = get_fop (bp, ops);
    double d = *get_dop (bp, ops + 1);

    *r = (float) d;
    END_INSN;
  }
  CASE (MIR_D2LD, 2) {
    long double *r = get_ldop (bp, ops);
    double d = *get_dop (bp, ops + 1);

    *r = d;
    END_INSN;
  }
  CASE (MIR_LD2F, 2) {
    float *r = get_fop (bp, ops);
    long double ld = *get_ldop (bp, ops + 1);

    *r = (float) ld;
    END_INSN;
  }

  CASE (MIR_LD2D, 2) {
    double *r = get_dop (bp, ops);
    long double ld = *get_ldop (bp, ops + 1);

    *r = ld;
    END_INSN;
  }

  SCASE (MIR_NEG, 2, IOP2 (-));
  SCASE (MIR_NEGS, 2, IOP2S (-));
  SCASE (MIR_FNEG, 2, FOP2 (-));
  SCASE (MIR_DNEG, 2, DOP2 (-));
  SCASE (MIR_LDNEG, 2, LDOP2 (-));

  CASE (MIR_ADDR8, 2) {
    offset = addr_offset8;
    goto common_addr;
  }
  CASE (MIR_ADDR16, 2) {
    offset = addr_offset16;
    goto common_addr;
  }
  CASE (MIR_ADDR32, 2) {
    offset = addr_offset32;
    goto common_addr;
  }
  CASE (MIR_ADDR, 2)
  offset = 0;
common_addr:;
  {
    int64_t *r = get_iop (bp, ops);
    void **p = get_aop (bp, ops + 1);

    *r = (int64_t) p + offset;
    END_INSN;
  }

  SCASE (MIR_ADD, 3, IOP3 (+));
  SCASE (MIR_ADDS, 3, IOP3S (+));
  SCASE (MIR_FADD, 3, FOP3 (+));
  SCASE (MIR_DADD, 3, DOP3 (+));
  SCASE (MIR_LDADD, 3, LDOP3 (+));

  SCASE (MIR_SUB, 3, IOP3 (-));
  SCASE (MIR_SUBS, 3, IOP3S (-));
  SCASE (MIR_FSUB, 3, FOP3 (-));
  SCASE (MIR_DSUB, 3, DOP3 (-));
  SCASE (MIR_LDSUB, 3, LDOP3 (-));

  SCASE (MIR_MUL, 3, IOP3 (*));
  SCASE (MIR_MULS, 3, IOP3S (*));
  SCASE (MIR_FMUL, 3, FOP3 (*));
  SCASE (MIR_DMUL, 3, DOP3 (*));
  SCASE (MIR_LDMUL, 3, LDOP3 (*));

  SCASE (MIR_DIV, 3, IOP3 (/));
  SCASE (MIR_DIVS, 3, IOP3S (/));
  SCASE (MIR_UDIV, 3, UOP3 (/));
  SCASE (MIR_UDIVS, 3, UOP3S (/));
  SCASE (MIR_FDIV, 3, FOP3 (/));
  SCASE (MIR_DDIV, 3, DOP3 (/));
  SCASE (MIR_LDDIV, 3, LDOP3 (/));

  SCASE (MIR_MOD, 3, IOP3 (%));
  SCASE (MIR_MODS, 3, IOP3S (%));
  SCASE (MIR_UMOD, 3, UOP3 (%));
  SCASE (MIR_UMODS, 3, UOP3S (%));

  SCASE (MIR_AND, 3, IOP3 (&));
  SCASE (MIR_ANDS, 3, IOP3S (&));
  SCASE (MIR_OR, 3, IOP3 (|));
  SCASE (MIR_ORS, 3, IOP3S (|));
  SCASE (MIR_XOR, 3, IOP3 (^));
  SCASE (MIR_XORS, 3, IOP3S (^));
  SCASE (MIR_LSH, 3, IOP3 (<<));
  SCASE (MIR_LSHS, 3, IOP3S (<<));

  SCASE (MIR_RSH, 3, IOP3 (>>));
  SCASE (MIR_RSHS, 3, IOP3S (>>));
  SCASE (MIR_URSH, 3, UIOP3 (>>));
  SCASE (MIR_URSHS, 3, UIOP3S (>>));

  SCASE (MIR_EQ, 3, ICMP (==));
  SCASE (MIR_EQS, 3, ICMPS (==));
  SCASE (MIR_FEQ, 3, FCMP (==));
  SCASE (MIR_DEQ, 3, DCMP (==));
  SCASE (MIR_LDEQ, 3, LDCMP (==));

  SCASE (MIR_NE, 3, ICMP (!=));
  SCASE (MIR_NES, 3, ICMPS (!=));
  SCASE (MIR_FNE, 3, FCMP (!=));
  SCASE (MIR_DNE, 3, DCMP (!=));
  SCASE (MIR_LDNE, 3, LDCMP (!=));

  SCASE (MIR_LT, 3, ICMP (<));
  SCASE (MIR_LTS, 3, ICMPS (<));
  SCASE (MIR_ULT, 3, UCMP (<));
  SCASE (MIR_ULTS, 3, UCMPS (<));
  SCASE (MIR_FLT, 3, FCMP (<));
  SCASE (MIR_DLT, 3, DCMP (<));
  SCASE (MIR_LDLT, 3, LDCMP (<));

  SCASE (MIR_LE, 3, ICMP (<=));
  SCASE (MIR_LES, 3, ICMPS (<=));
  SCASE (MIR_ULE, 3, UCMP (<=));
  SCASE (MIR_ULES, 3, UCMPS (<=));
  SCASE (MIR_FLE, 3, FCMP (<=));
  SCASE (MIR_DLE, 3, DCMP (<=));
  SCASE (MIR_LDLE, 3, LDCMP (<=));

  SCASE (MIR_GT, 3, ICMP (>));
  SCASE (MIR_GTS, 3, ICMPS (>));
  SCASE (MIR_UGT, 3, UCMP (>));
  SCASE (MIR_UGTS, 3, UCMPS (>));
  SCASE (MIR_FGT, 3, FCMP (>));
  SCASE (MIR_DGT, 3, DCMP (>));
  SCASE (MIR_LDGT, 3, LDCMP (>));

  SCASE (MIR_GE, 3, ICMP (>=));
  SCASE (MIR_GES, 3, ICMPS (>=));
  SCASE (MIR_UGE, 3, UCMP (>=));
  SCASE (MIR_UGES, 3, UCMPS (>=));
  SCASE (MIR_FGE, 3, FCMP (>=));
  SCASE (MIR_DGE, 3, DCMP (>=));
  SCASE (MIR_LDGE, 3, LDCMP (>=));

  CASE (MIR_ADDO, 3) {
    int64_t *r = get_iop (bp, ops);
    int64_t op1 = *get_iop (bp, ops + 1), op2 = *get_iop (bp, ops + 2);
    unsigned_overflow_p = (uint64_t) op1 > UINT64_MAX - (uint64_t) op2;
    signed_overflow_p = op2 >= 0 ? op1 > INT64_MAX - op2 : op1 < INT64_MIN - op2;
    *r = op1 + op2;
    END_INSN;
  }

  CASE (MIR_ADDOS, 3) {
    int64_t *r = get_iop (bp, ops);
    int32_t op1 = (int32_t) *get_iop (bp, ops + 1), op2 = (int32_t) *get_iop (bp, ops + 2);
    unsigned_overflow_p = (uint32_t) op1 > UINT32_MAX - (uint32_t) op2;
    signed_overflow_p = op2 >= 0 ? op1 > INT32_MAX - op2 : op1 < INT32_MIN - op2;
    *r = op1 + op2;
    END_INSN;
  }

  CASE (MIR_SUBO, 3) {
    int64_t *r = get_iop (bp, ops);
    int64_t op1 = *get_iop (bp, ops + 1), op2 = *get_iop (bp, ops + 2);
    unsigned_overflow_p = (uint64_t) op1 < (uint64_t) op2;
    signed_overflow_p = op2 < 0 ? op1 > INT64_MAX + op2 : op1 < INT64_MIN + op2;
    *r = op1 - op2;
    END_INSN;
  }

  CASE (MIR_SUBOS, 3) {
    int64_t *r = get_iop (bp, ops);
    int32_t op1 = (int32_t) *get_iop (bp, ops + 1), op2 = (int32_t) *get_iop (bp, ops + 2);
    unsigned_overflow_p = (uint32_t) op1 < (uint32_t) op2;
    signed_overflow_p = op2 < 0 ? op1 > INT32_MAX + op2 : op1 < INT32_MIN + op2;
    *r = op1 - op2;
    END_INSN;
  }

  CASE (MIR_MULO, 3) {
    int64_t *r = get_iop (bp, ops);
    int64_t op1 = *get_iop (bp, ops + 1), op2 = *get_iop (bp, ops + 2);
    signed_overflow_p = (op1 == 0    ? FALSE
                         : op1 == -1 ? op2 < -INT64_MAX
                         : op1 > 0   ? (op2 > 0 ? INT64_MAX / op1 < op2 : INT64_MIN / op1 > op2)
                                     : (op2 > 0 ? INT64_MIN / op1 < op2 : INT64_MAX / op1 > op2));
    *r = op1 * op2;
    END_INSN;
  }

  CASE (MIR_MULOS, 3) {
    int64_t *r = get_iop (bp, ops);
    int32_t op1 = (int32_t) *get_iop (bp, ops + 1), op2 = (int32_t) *get_iop (bp, ops + 2);
    signed_overflow_p = (op1 == 0    ? FALSE
                         : op1 == -1 ? op2 < -INT32_MAX
                         : op1 > 0   ? (op2 > 0 ? INT32_MAX / op1 < op2 : INT32_MIN / op1 > op2)
                                     : (op2 > 0 ? INT32_MIN / op1 < op2 : INT32_MAX / op1 > op2));
    *r = op1 * op2;
    END_INSN;
  }

  CASE (MIR_UMULO, 3) {
    uint64_t *r = get_uop (bp, ops);
    uint64_t op1 = *get_uop (bp, ops + 1), op2 = *get_uop (bp, ops + 2);
    unsigned_overflow_p = op1 == 0 ? FALSE : UINT64_MAX / op1 < op2;
    *r = op1 * op2;
    END_INSN;
  }

  CASE (MIR_UMULOS, 3) {
    uint64_t *r = get_uop (bp, ops);
    uint32_t op1 = (uint32_t) *get_uop (bp, ops + 1), op2 = (uint32_t) *get_uop (bp, ops + 2);
    unsigned_overflow_p = op1 == 0 ? FALSE : UINT32_MAX / op1 < op2;
    *r = op1 * op2;
    END_INSN;
  }

  SCASE (MIR_JMP, 1, pc = code + get_i (ops));
  CASE (MIR_BT, 2) {
    int64_t cond = *get_iop (bp, ops + 1);

    if (cond) pc = code + get_i (ops);
    END_INSN;
  }
  CASE (MIR_BF, 2) {
    int64_t cond = *get_iop (bp, ops + 1);

    if (!cond) pc = code + get_i (ops);
    END_INSN;
  }
  CASE (MIR_BTS, 2) {
    int32_t cond = (int32_t) *get_iop (bp, ops + 1);

    if (cond) pc = code + get_i (ops);
    END_INSN;
  }
  CASE (MIR_BFS, 2) {
    int32_t cond = (int32_t) *get_iop (bp, ops + 1);

    if (!cond) pc = code + get_i (ops);
    END_INSN;
  }
  SCASE (MIR_BEQ, 3, BICMP (==));
  SCASE (MIR_BEQS, 3, BICMPS (==));
  SCASE (MIR_FBEQ, 3, BFCMP (==));
  SCASE (MIR_DBEQ, 3, BDCMP (==));
  SCASE (MIR_LDBEQ, 3, BLDCMP (==));
  SCASE (MIR_BNE, 3, BICMP (!=));
  SCASE (MIR_BNES, 3, BICMPS (!=));
  SCASE (MIR_FBNE, 3, BFCMP (!=));
  SCASE (MIR_DBNE, 3, BDCMP (!=));
  SCASE (MIR_LDBNE, 3, BLDCMP (!=));
  SCASE (MIR_BLT, 3, BICMP (<));
  SCASE (MIR_BLTS, 3, BICMPS (<));
  SCASE (MIR_UBLT, 3, BUCMP (<));
  SCASE (MIR_UBLTS, 3, BUCMPS (<));
  SCASE (MIR_FBLT, 3, BFCMP (<));
  SCASE (MIR_DBLT, 3, BDCMP (<));
  SCASE (MIR_LDBLT, 3, BLDCMP (<));
  SCASE (MIR_BLE, 3, BICMP (<=));
  SCASE (MIR_BLES, 3, BICMPS (<=));
  SCASE (MIR_UBLE, 3, BUCMP (<=));
  SCASE (MIR_UBLES, 3, BUCMPS (<=));
  SCASE (MIR_FBLE, 3, BFCMP (<=));
  SCASE (MIR_DBLE, 3, BDCMP (<=));
  SCASE (MIR_LDBLE, 3, BLDCMP (<=));
  SCASE (MIR_BGT, 3, BICMP (>));
  SCASE (MIR_BGTS, 3, BICMPS (>));
  SCASE (MIR_UBGT, 3, BUCMP (>));
  SCASE (MIR_UBGTS, 3, BUCMPS (>));
  SCASE (MIR_FBGT, 3, BFCMP (>));
  SCASE (MIR_DBGT, 3, BDCMP (>));
  SCASE (MIR_LDBGT, 3, BLDCMP (>));
  SCASE (MIR_BGE, 3, BICMP (>=));
  SCASE (MIR_BGES, 3, BICMPS (>=));
  SCASE (MIR_UBGE, 3, BUCMP (>=));
  SCASE (MIR_UBGES, 3, BUCMPS (>=));
  SCASE (MIR_FBGE, 3, BFCMP (>=));
  SCASE (MIR_DBGE, 3, BDCMP (>=));
  SCASE (MIR_LDBGE, 3, BLDCMP (>=));

  CASE (MIR_BO, 1) {
    if (signed_overflow_p) pc = code + get_i (ops);
    END_INSN;
  }
  CASE (MIR_UBO, 1) {
    if (unsigned_overflow_p) pc = code + get_i (ops);
    END_INSN;
  }
  CASE (MIR_BNO, 1) {
    if (!signed_overflow_p) pc = code + get_i (ops);
    END_INSN;
  }
  CASE (MIR_UBNO, 1) {
    if (!unsigned_overflow_p) pc = code + get_i (ops);
    END_INSN;
  }
  CASE (MIR_LADDR, 2) {
    void **r = get_aop (bp, ops);
    *r = code + get_i (ops + 1);
    END_INSN;
  }

  CASE (MIR_JMPI, 1) { /* jmpi thunk */
    void **r = get_aop (bp, ops);
    pc = *r;
    END_INSN;
  }

  CASE (MIR_CALL, 0) {
    int (*func_addr) (void *buf) = *get_aop (bp, ops + 4);

    if (func_addr != setjmp_addr) {
      pc = call_insn_execute (ctx, pc, bp, ops, FALSE);
    } else {
      int res;
      int64_t nops = get_i (ops); /* #args w/o nop, insn, and ff interface address */
      MIR_item_t proto_item = get_a (ops + 3);
      size_t start = proto_item->u.proto->nres + 5;
      bp[-2].a = pc;
      res = (*func_addr) (*get_aop (bp, ops + start));
      ops = pc = bp[-2].a;
      nops = get_i (ops);
      bp[get_i (ops + 5)].i = res;
      pc += nops + 3; /* nops itself, the call insn, add ff interface address */
    }
    END_INSN;
  }
  CASE (IC_IMM_CALL, 0) {
    int (*func_addr) (void *buf) = get_a (ops + 4);

    if (func_addr != setjmp_addr) {
      pc = call_insn_execute (ctx, pc, bp, ops, TRUE);
    } else {
      int res;
      int64_t nops = get_i (ops); /* #args w/o nop, insn, and ff interface address */
      MIR_item_t proto_item = get_a (ops + 3);
      size_t start = proto_item->u.proto->nres + 5;
      bp[-2].a = pc;
      res = (*func_addr) (*get_aop (bp, ops + start));
      ops = pc = bp[-2].a;
      nops = get_i (ops);
      bp[get_i (ops + 5)].i = res;
      pc += nops + 3; /* nops itself, the call insn, add ff interface address */
    }
    END_INSN;
  }

  SCASE (MIR_INLINE, 0, mir_assert (FALSE));

  CASE (MIR_JCALL, 0) {
    int (*func_addr) (void *buf) = *get_aop (bp, ops + 4);
    if (func_addr == setjmp_addr)
      (*MIR_get_error_func (ctx)) (MIR_invalid_insn_error, "jcall of setjmp");
    call_insn_execute (ctx, pc, bp, ops, FALSE);
    pc = jret_addr;
    END_INSN;
  }
  CASE (IC_IMM_JCALL, 0) {
    int (*func_addr) (void *buf) = get_a (ops + 4);
    if (func_addr == setjmp_addr)
      (*MIR_get_error_func (ctx)) (MIR_invalid_insn_error, "jcall of setjmp");
    call_insn_execute (ctx, pc, bp, ops, TRUE);
    pc = jret_addr;
    END_INSN;
  }

  CASE (MIR_SWITCH, 0) {
    int64_t nops = get_i (ops); /* #ops */
    int64_t index = *get_iop (bp, ops + 1);

    mir_assert (index + 1 < nops);
    pc = code + get_i (ops + index + 2);
    END_INSN;
  }

  CASE (MIR_RET, 0) {
    int64_t nops = get_i (ops); /* #ops */
    for (int64_t i = 0; i < nops; i++) results[i] = bp[get_i (ops + i + 1)];
    pc += nops + 1;
    return;
    END_INSN;
  }

  CASE (MIR_JRET, 0) {
    jret_addr = bp[get_i (ops)].a; /* pc for continuation */
    return;
    END_INSN;
  }

  CASE (MIR_ALLOCA, 2) {
    int64_t *r, s;

    r = get_2iops (bp, ops, &s);
    *r = (uint64_t) alloca (s);
    END_INSN;
  }
  CASE (MIR_BSTART, 1) {
    void **p = get_aop (bp, ops);

    *p = bstart_builtin ();
    END_INSN;
  }
  SCASE (MIR_BEND, 1, bend_builtin (*get_aop (bp, ops)));
  CASE (MIR_VA_ARG, 3) {
    int64_t *r, va, tp;

    r = get_2iops (bp, ops, &va);
    tp = get_i (ops + 2);
    *r = (uint64_t) va_arg_builtin ((void *) va, tp);
    END_INSN;
  }
  CASE (MIR_VA_BLOCK_ARG, 4) {
    int64_t *r, va, size;

    r = get_3iops (bp, ops, &va, &size);
    va_block_arg_builtin ((void *) *r, (void *) va, size, *get_iop (bp, ops + 3));
    END_INSN;
  }
  SCASE (MIR_VA_START, 1, va_start_interp_builtin (ctx, bp[get_i (ops)].a, bp[-1].a));
  SCASE (MIR_VA_END, 1, va_end_interp_builtin (ctx, bp[get_i (ops)].a));

  SCASE (IC_LDI8, 2, LD (iop, int64_t, int8_t));
  SCASE (IC_LDU8, 2, LD (uop, uint64_t, uint8_t));
  SCASE (IC_LDI16, 2, LD (iop, int64_t, int16_t));
  SCASE (IC_LDU16, 2, LD (uop, uint64_t, uint16_t));
  SCASE (IC_LDI32, 2, LD (iop, int64_t, int32_t));
  SCASE (IC_LDU32, 2, LD (uop, uint64_t, uint32_t));
  SCASE (IC_LDI64, 2, LD (iop, int64_t, int64_t));
  SCASE (IC_LDF, 2, LD (fop, float, float));
  SCASE (IC_LDD, 2, LD (dop, double, double));
  SCASE (IC_LDLD, 2, LD (ldop, long double, long double));
  CASE (IC_MOVP, 2) {
    void **r = get_aop (bp, ops), *a = get_a (ops + 1);
    *r = a;
    END_INSN;
  }
  SCASE (IC_STI8, 2, ST (iop, int64_t, int8_t));
  SCASE (IC_STU8, 2, ST (iop, uint64_t, uint8_t));
  SCASE (IC_STI16, 2, ST (iop, int64_t, int16_t));
  SCASE (IC_STU16, 2, ST (iop, uint64_t, uint16_t));
  SCASE (IC_STI32, 2, ST (iop, int64_t, int32_t));
  SCASE (IC_STU32, 2, ST (iop, uint64_t, uint32_t));
  SCASE (IC_STI64, 2, ST (iop, int64_t, int64_t));
  SCASE (IC_STF, 2, ST (fop, float, float));
  SCASE (IC_STD, 2, ST (dop, double, double));
  SCASE (IC_STLD, 2, ST (ldop, long double, long double));
  CASE (IC_MOVI, 2) {
    int64_t *r = get_iop (bp, ops), imm = get_i (ops + 1);
    *r = imm;
    END_INSN;
  }
  CASE (IC_MOVF, 2) {
    float *r = get_fop (bp, ops), imm = get_f (ops + 1);
    *r = imm;
    END_INSN;
  }
  CASE (IC_MOVD, 2) {
    double *r = get_dop (bp, ops), imm = get_d (ops + 1);
    *r = imm;
    END_INSN;
  }
  CASE (IC_MOVLD, 2) {
    long double *r = get_ldop (bp, ops), imm = get_ld (ops + 1);
    *r = imm;
    END_INSN;
  }
#if !DIRECT_THREADED_DISPATCH
default: mir_assert (FALSE);
}
}
#endif
}

static inline func_desc_t get_func_desc (MIR_item_t func_item) {
  mir_assert (func_item->item_type == MIR_func_item);
  return func_item->data;
}

static htab_hash_t ff_interface_hash (ff_interface_t i, void *arg MIR_UNUSED) {
  htab_hash_t h = (htab_hash_t) mir_hash_step (mir_hash_init (0), i->nres);
  h = (htab_hash_t) mir_hash_step (h, i->nargs);
  h = (htab_hash_t) mir_hash_step (h, i->arg_vars_num);
  h = (htab_hash_t) mir_hash (i->res_types, sizeof (MIR_type_t) * i->nres, h);
  for (size_t n = 0; n < i->nargs; n++) {
    h = (htab_hash_t) mir_hash_step (h, i->arg_descs[n].type);
    if (MIR_all_blk_type_p (i->arg_descs[n].type))
      h = (htab_hash_t) mir_hash_step (h, i->arg_descs[n].size);
  }
  return (htab_hash_t) mir_hash_finish (h);
}

static int ff_interface_eq (ff_interface_t i1, ff_interface_t i2, void *arg MIR_UNUSED) {
  if (i1->nres != i2->nres || i1->nargs != i2->nargs || i1->arg_vars_num != i2->arg_vars_num)
    return FALSE;
  if (memcmp (i1->res_types, i2->res_types, sizeof (MIR_type_t) * i1->nres) != 0) return FALSE;
  for (size_t n = 0; n < i1->nargs; n++) {
    if (i1->arg_descs[n].type != i2->arg_descs[n].type) return FALSE;
    if (MIR_all_blk_type_p (i1->arg_descs[n].type)
        && i1->arg_descs[n].size != i2->arg_descs[n].size)
      return FALSE;
  }
  return TRUE;
}

static void ff_interface_clear (ff_interface_t ffi, void *arg) {
  MIR_alloc_t alloc = (MIR_alloc_t) arg;
  MIR_free (alloc, ffi);
}

static void *get_ff_interface (MIR_context_t ctx, size_t arg_vars_num, size_t nres,
                               MIR_type_t *res_types, size_t nargs, _MIR_arg_desc_t *arg_descs,
                               int vararg_p MIR_UNUSED) {
  struct interp_ctx *interp_ctx = ctx->interp_ctx;
  struct ff_interface ffi_s;
  ff_interface_t tab_ffi, ffi;
  int htab_res;

  ffi_s.arg_vars_num = arg_vars_num;
  ffi_s.nres = nres;
  ffi_s.nargs = nargs;
  ffi_s.res_types = res_types;
  ffi_s.arg_descs = arg_descs;
  if (HTAB_DO (ff_interface_t, ff_interface_tab, &ffi_s, HTAB_FIND, tab_ffi))
    return tab_ffi->interface_addr;
  ffi = MIR_malloc (ctx->alloc, sizeof (struct ff_interface) + sizeof (_MIR_arg_desc_t) * nargs
                    + sizeof (MIR_type_t) * nres);
  ffi->arg_vars_num = arg_vars_num;
  ffi->nres = nres;
  ffi->nargs = nargs;
  ffi->arg_descs = (_MIR_arg_desc_t *) ((char *) ffi + sizeof (struct ff_interface));
  ffi->res_types = (MIR_type_t *) ((char *) ffi->arg_descs + nargs * sizeof (_MIR_arg_desc_t));
  memcpy (ffi->res_types, res_types, sizeof (MIR_type_t) * nres);
  memcpy (ffi->arg_descs, arg_descs, sizeof (_MIR_arg_desc_t) * nargs);
  ffi->interface_addr
    = _MIR_get_ff_call (ctx, nres, res_types, nargs, call_arg_descs, arg_vars_num);
  htab_res = HTAB_DO (ff_interface_t, ff_interface_tab, ffi, HTAB_INSERT, tab_ffi);
  mir_assert (!htab_res && ffi == tab_ffi);
  return ffi->interface_addr;
}

static void call (MIR_context_t ctx, MIR_val_t *bp, MIR_op_t *insn_arg_ops, code_t ffi_address_ptr,
                  MIR_item_t proto_item, void *addr, code_t res_ops, size_t nargs) {
  struct interp_ctx *interp_ctx = ctx->interp_ctx;
  size_t i, arg_vars_num, nres;
  MIR_val_t *res;
  MIR_type_t type;
  MIR_var_t *arg_vars = NULL;
  MIR_proto_t proto = proto_item->u.proto;
  MIR_op_mode_t mode;
  void *ff_interface_addr;

  if (proto->args == NULL) {
    mir_assert (nargs == 0 && !proto->vararg_p);
    arg_vars_num = 0;
  } else {
    mir_assert (nargs >= VARR_LENGTH (MIR_var_t, proto->args)
                && (proto->vararg_p || nargs == VARR_LENGTH (MIR_var_t, proto->args)));
    arg_vars = VARR_ADDR (MIR_var_t, proto->args);
    arg_vars_num = VARR_LENGTH (MIR_var_t, proto->args);
  }
  nres = proto->nres;
  if (VARR_EXPAND (MIR_val_t, call_res_args_varr, nargs + nres)
      || VARR_EXPAND (_MIR_arg_desc_t, call_arg_descs_varr, nargs)) {
    call_res_args = VARR_ADDR (MIR_val_t, call_res_args_varr);
    call_arg_descs = VARR_ADDR (_MIR_arg_desc_t, call_arg_descs_varr);
  }
  if ((ff_interface_addr = ffi_address_ptr->a) == NULL) {
    for (i = 0; i < nargs; i++) {
      if (i < arg_vars_num) {
        call_arg_descs[i].type = arg_vars[i].type;
        if (MIR_all_blk_type_p (arg_vars[i].type)) call_arg_descs[i].size = arg_vars[i].size;
        continue;
      }
      if (insn_arg_ops[i].mode == MIR_OP_MEM) { /* (r)block arg */
        mir_assert (MIR_all_blk_type_p (insn_arg_ops[i].u.mem.type));
        call_arg_descs[i].type = insn_arg_ops[i].u.mem.type;
        call_arg_descs[i].size = insn_arg_ops[i].u.mem.disp;
      } else {
        mode = insn_arg_ops[i].value_mode;
        mir_assert (mode == MIR_OP_INT || mode == MIR_OP_UINT || mode == MIR_OP_FLOAT
                    || mode == MIR_OP_DOUBLE || mode == MIR_OP_LDOUBLE);
        if (mode == MIR_OP_FLOAT)
          (*MIR_get_error_func (ctx)) (MIR_call_op_error,
                                       "passing float variadic arg (should be passed as double)");
        call_arg_descs[i].type = (mode == MIR_OP_DOUBLE    ? MIR_T_D
                                  : mode == MIR_OP_LDOUBLE ? MIR_T_LD
                                                           : MIR_T_I64);
      }
    }
    ff_interface_addr = ffi_address_ptr->a
      = get_ff_interface (ctx, arg_vars_num, nres, proto->res_types, nargs, call_arg_descs,
                          proto->vararg_p);
  }

  for (i = 0; i < nargs; i++) {
    if (i >= arg_vars_num) {
      call_res_args[i + nres] = arg_vals[i];
      continue;
    }
    type = arg_vars[i].type;
    switch (type) {
    case MIR_T_I8: call_res_args[i + nres].i = (int8_t) (arg_vals[i].i); break;
    case MIR_T_U8: call_res_args[i + nres].u = (uint8_t) (arg_vals[i].i); break;
    case MIR_T_I16: call_res_args[i + nres].i = (int16_t) (arg_vals[i].i); break;
    case MIR_T_U16: call_res_args[i + nres].u = (uint16_t) (arg_vals[i].i); break;
    case MIR_T_I32: call_res_args[i + nres].i = (int32_t) (arg_vals[i].i); break;
    case MIR_T_U32: call_res_args[i + nres].u = (uint32_t) (arg_vals[i].i); break;
    case MIR_T_I64: call_res_args[i + nres].i = (int64_t) (arg_vals[i].i); break;
    case MIR_T_U64: call_res_args[i + nres].u = (uint64_t) (arg_vals[i].i); break;
    case MIR_T_F: call_res_args[i + nres].f = arg_vals[i].f; break;
    case MIR_T_D: call_res_args[i + nres].d = arg_vals[i].d; break;
    case MIR_T_LD: call_res_args[i + nres].ld = arg_vals[i].ld; break;
    case MIR_T_P: call_res_args[i + nres].u = (uint64_t) arg_vals[i].a; break;
    default:
      mir_assert (MIR_all_blk_type_p (type));
      call_res_args[i + nres].u = (uint64_t) arg_vals[i].a;
      break;
    }
  }
  ((void (*) (void *, void *)) ff_interface_addr) (addr, call_res_args); /* call */
  for (i = 0; i < nres; i++) {
    res = &bp[get_i (res_ops + i)];
    switch (proto->res_types[i]) {
    case MIR_T_I8: res->i = (int8_t) (call_res_args[i].i); break;
    case MIR_T_U8: res->u = (uint8_t) (call_res_args[i].u); break;
    case MIR_T_I16: res->i = (int16_t) (call_res_args[i].i); break;
    case MIR_T_U16: res->u = (uint16_t) (call_res_args[i].u); break;
    case MIR_T_I32: res->i = (int32_t) (call_res_args[i].i); break;
    case MIR_T_U32: res->u = (uint32_t) (call_res_args[i].u); break;
    case MIR_T_I64: res->i = (int64_t) (call_res_args[i].i); break;
    case MIR_T_U64: res->u = (uint64_t) (call_res_args[i].u); break;
    case MIR_T_F: res->f = call_res_args[i].f; break;
    case MIR_T_D: res->d = call_res_args[i].d; break;
    case MIR_T_LD: res->ld = call_res_args[i].ld; break;
    case MIR_T_P: res->a = call_res_args[i].a; break;
    default: mir_assert (FALSE);
    }
  }
}

static void interp_init (MIR_context_t ctx) {
  MIR_alloc_t alloc = ctx->alloc;
  struct interp_ctx *interp_ctx;

  addr_offset8 = _MIR_addr_offset (ctx, MIR_ADDR8);
  addr_offset16 = _MIR_addr_offset (ctx, MIR_ADDR16);
  addr_offset32 = _MIR_addr_offset (ctx, MIR_ADDR32);
  if ((interp_ctx = ctx->interp_ctx = MIR_malloc (alloc, sizeof (struct interp_ctx))) == NULL)
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for ctx");
#if DIRECT_THREADED_DISPATCH
  eval (ctx, NULL, NULL, NULL);
#endif
  VARR_CREATE (MIR_insn_t, branches, alloc, 0);
  VARR_CREATE (MIR_val_t, code_varr, alloc, 0);
  VARR_CREATE (MIR_val_t, arg_vals_varr, alloc, 0);
  arg_vals = VARR_ADDR (MIR_val_t, arg_vals_varr);
  VARR_CREATE (MIR_val_t, call_res_args_varr, alloc, 0);
  VARR_CREATE (_MIR_arg_desc_t, call_arg_descs_varr, alloc, 0);
  call_res_args = VARR_ADDR (MIR_val_t, call_res_args_varr);
  call_arg_descs = VARR_ADDR (_MIR_arg_desc_t, call_arg_descs_varr);
  HTAB_CREATE_WITH_FREE_FUNC (ff_interface_t, ff_interface_tab, alloc, 1000, ff_interface_hash,
                              ff_interface_eq, ff_interface_clear, alloc);
#if MIR_INTERP_TRACE
  trace_insn_ident = 0;
#endif
  bstart_builtin = _MIR_get_bstart_builtin (ctx);
  bend_builtin = _MIR_get_bend_builtin (ctx);
}

static void interp_finish (MIR_context_t ctx) {
  struct interp_ctx *interp_ctx = ctx->interp_ctx;

  VARR_DESTROY (MIR_insn_t, branches);
  VARR_DESTROY (MIR_val_t, code_varr);
  VARR_DESTROY (MIR_val_t, arg_vals_varr);
  VARR_DESTROY (MIR_val_t, call_res_args_varr);
  VARR_DESTROY (_MIR_arg_desc_t, call_arg_descs_varr);
  HTAB_DESTROY (ff_interface_t, ff_interface_tab);
  /* Clear func descs???  */
  MIR_free (ctx->alloc, ctx->interp_ctx);
  ctx->interp_ctx = NULL;
}

#if VA_LIST_IS_ARRAY_P
typedef va_list va_t;
#else
    typedef va_list *va_t;
#endif

static void interp_arr_varg (MIR_context_t ctx, MIR_item_t func_item, MIR_val_t *results,
                             size_t nargs, MIR_val_t *vals, va_t va) {
  func_desc_t func_desc;
  MIR_val_t *bp;

  mir_assert (func_item->item_type == MIR_func_item);
  if (func_item->data == NULL) generate_icode (ctx, func_item);
  func_desc = get_func_desc (func_item);
  bp = alloca ((func_desc->nregs + 2) * sizeof (MIR_val_t));
  bp++; /* reserved for setjmp/longjmp */
  bp[0].a = va;
  bp++;
  if (func_desc->nregs < nargs + 1) nargs = func_desc->nregs - 1;
  bp[0].i = 0;
  memcpy (&bp[1], vals, sizeof (MIR_val_t) * nargs);
  eval (ctx, func_desc, bp, results);
  if (va != NULL)
#if VA_LIST_IS_ARRAY_P
    va_end (va);
#else
        va_end (*va);
#endif
}

void MIR_interp (MIR_context_t ctx, MIR_item_t func_item, MIR_val_t *results, size_t nargs, ...) {
  struct interp_ctx *interp_ctx = ctx->interp_ctx;
  va_list argp;
  size_t i;

  if (VARR_EXPAND (MIR_val_t, arg_vals_varr, nargs))
    arg_vals = VARR_ADDR (MIR_val_t, arg_vals_varr);
  va_start (argp, nargs);
  for (i = 0; i < nargs; i++) arg_vals[i] = va_arg (argp, MIR_val_t);
#if VA_LIST_IS_ARRAY_P
  interp_arr_varg (ctx, func_item, results, nargs, arg_vals, argp);
#else
      interp_arr_varg (ctx, func_item, results, nargs, arg_vals, (va_t) &argp);
#endif
}

void MIR_interp_arr_varg (MIR_context_t ctx, MIR_item_t func_item, MIR_val_t *results, size_t nargs,
                          MIR_val_t *vals, va_list va) {
  func_desc_t func_desc;
  MIR_val_t *bp;

  mir_assert (func_item->item_type == MIR_func_item);
  if (func_item->data == NULL) generate_icode (ctx, func_item);
  func_desc = get_func_desc (func_item);
  bp = alloca ((func_desc->nregs + 2) * sizeof (MIR_val_t));
  bp++; /* reserved for setjmp/longjmp */
#if VA_LIST_IS_ARRAY_P
  bp[0].a = va;
#else
      bp[0].a = &va;
#endif
  bp++;
  if (func_desc->nregs < nargs + 1) nargs = func_desc->nregs - 1;
  bp[0].i = 0;
  memcpy (&bp[1], vals, sizeof (MIR_val_t) * nargs);
  eval (ctx, func_desc, bp, results);
}

void MIR_interp_arr (MIR_context_t ctx, MIR_item_t func_item, MIR_val_t *results, size_t nargs,
                     MIR_val_t *vals) {
  interp_arr_varg (ctx, func_item, results, nargs, vals, NULL);
}

/* C call interface to interpreter.  It is based on knowledge of
   common vararg implementation.  For some targets it might not
   work.  */
static void interp (MIR_context_t ctx, MIR_item_t func_item, va_list va, MIR_val_t *results) {
  struct interp_ctx *interp_ctx = ctx->interp_ctx;
  size_t nargs;
  MIR_var_t *arg_vars;
  MIR_func_t func = func_item->u.func;

  nargs = func->nargs;
  arg_vars = VARR_ADDR (MIR_var_t, func->vars);
  if (VARR_EXPAND (MIR_val_t, arg_vals_varr, nargs))
    arg_vals = VARR_ADDR (MIR_val_t, arg_vals_varr);
  for (size_t i = 0; i < nargs; i++) {
    MIR_type_t type = arg_vars[i].type;
    switch (type) {
    case MIR_T_I8: arg_vals[i].i = (int8_t) va_arg (va, int32_t); break;
    case MIR_T_I16: arg_vals[i].i = (int16_t) va_arg (va, int32_t); break;
    case MIR_T_I32: arg_vals[i].i = va_arg (va, int32_t); break;
    case MIR_T_I64: arg_vals[i].i = va_arg (va, int64_t); break;
    case MIR_T_U8: arg_vals[i].i = (uint8_t) va_arg (va, uint32_t); break;
    case MIR_T_U16: arg_vals[i].i = (uint16_t) va_arg (va, uint32_t); break;
    case MIR_T_U32: arg_vals[i].i = va_arg (va, uint32_t); break;
    case MIR_T_U64: arg_vals[i].i = va_arg (va, uint64_t); break;
    case MIR_T_F: {
      union {
        double d;
        float f;
      } u;
      u.d = va_arg (va, double);
#if defined(__PPC64__)
      arg_vals[i].f = u.d;
#else
          arg_vals[i].f = u.f;
#endif
      break;
    }
    case MIR_T_D: arg_vals[i].d = va_arg (va, double); break;
    case MIR_T_LD: arg_vals[i].ld = va_arg (va, long double); break;
    case MIR_T_P:
    case MIR_T_RBLK: arg_vals[i].a = va_arg (va, void *); break;
    default: mir_assert (MIR_blk_type_p (type)); arg_vals[i].a = alloca (arg_vars[i].size);
#if defined(__PPC64__) || defined(__aarch64__) || defined(__riscv) || defined(_WIN32)
      va_block_arg_builtin (arg_vals[i].a, &va, arg_vars[i].size, type - MIR_T_BLK);
#else
          va_block_arg_builtin (arg_vals[i].a, va, arg_vars[i].size, type - MIR_T_BLK);
#endif
      break;
    }
  }
#if VA_LIST_IS_ARRAY_P
  interp_arr_varg (ctx, func_item, results, nargs, arg_vals, va);
#else
      interp_arr_varg (ctx, func_item, results, nargs, arg_vals, (va_t) &va);
#endif
}

static void redirect_interface_to_interp (MIR_context_t ctx, MIR_item_t func_item) {
  _MIR_redirect_thunk (ctx, func_item->addr, _MIR_get_interp_shim (ctx, func_item, interp));
}

void MIR_set_interp_interface (MIR_context_t ctx, MIR_item_t func_item) {
  if (func_item != NULL) redirect_interface_to_interp (ctx, func_item);
}

#endif /* #ifdef MIR_NO_INTERP */
