/* This file is a part of MIR project.
   Copyright (C) 2019-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.

   Translator of LLVM bitcode into MIR.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "llvm2mir.h"
#include "mir-hash.h"
#include <llvm-c/Target.h>

static MIR_context_t context;
static MIR_module_t curr_mir_module;
static MIR_item_t curr_mir_func;
static unsigned curr_mir_func_reg_num;
static MIR_reg_t mir_int_temp_reg;

static void error (const char *message) {
  fprintf (stderr, "%s\n", message);
  exit (1);
}

static LLVMTargetDataRef TD;

typedef struct edge_phi_op_eval *edge_phi_op_eval_t;

DEF_DLIST_LINK (edge_phi_op_eval_t);

struct edge_phi_op_eval { /* edge insns for one phi variable  */
  DLIST (MIR_insn_t) insns;
  DLIST_LINK (edge_phi_op_eval_t) phi_op_eval_link;
};

DEF_DLIST (edge_phi_op_eval_t, phi_op_eval_link);

DEF_VARR (MIR_insn_t);

typedef struct out_edge *out_edge_t;

DEF_DLIST_LINK (out_edge_t);

struct out_edge {
  LLVMBasicBlockRef bb_dest;
  VARR (MIR_insn_t) * br_insns;
  DLIST_LINK (out_edge_t) out_edge_link;
  DLIST (edge_phi_op_eval_t) op_evals;
};

DEF_DLIST (out_edge_t, out_edge_link);

struct bb_gen_info {
  LLVMBasicBlockRef bb; /* bb in the curent func */
  MIR_label_t label;    /* MIR label for bb */
  MIR_insn_t last;      /* Last MIR insn for the block */
  DLIST (out_edge_t) out_edges;
};

typedef struct bb_gen_info *bb_gen_info_t;

DEF_VARR (bb_gen_info_t);
static VARR (bb_gen_info_t) * bb_gen_infos;

DEF_HTAB (bb_gen_info_t);
static HTAB (bb_gen_info_t) * bb_gen_info_tab;

static int bb_gen_info_eq (bb_gen_info_t bb_gen_info1, bb_gen_info_t bb_gen_info2, void *arg) {
  return bb_gen_info1->bb == bb_gen_info2->bb;
}
static htab_hash_t bb_gen_info_hash (bb_gen_info_t bb_gen_info, void *arg) {
  return mir_hash_finish (mir_hash_step (mir_hash_init (0x42), (uint64_t) bb_gen_info->bb));
}

static void init_bb_gen_info (void) {
  HTAB_CREATE (bb_gen_info_t, bb_gen_info_tab, 64, bb_gen_info_hash, bb_gen_info_eq, NULL);
  VARR_CREATE (bb_gen_info_t, bb_gen_infos, 0);
}

static void clear_bb_gen_info (bb_gen_info_t bb_gen_info) {
  edge_phi_op_eval_t bpi;
  out_edge_t e;

  while ((e = DLIST_HEAD (out_edge_t, bb_gen_info->out_edges)) != NULL) {
    DLIST_REMOVE (out_edge_t, bb_gen_info->out_edges, e);
    while ((bpi = DLIST_HEAD (edge_phi_op_eval_t, e->op_evals)) != NULL) {
      DLIST_REMOVE (edge_phi_op_eval_t, e->op_evals, bpi);
      free (bpi);
    }
    VARR_DESTROY (MIR_insn_t, e->br_insns);
    free (e);
  }
  free (bb_gen_info);
}

static void finish_bb_gen_info (void) {
  HTAB_DESTROY (bb_gen_info_t, bb_gen_info_tab);
  for (size_t i = 0; i < VARR_LENGTH (bb_gen_info_t, bb_gen_infos); i++)
    clear_bb_gen_info (VARR_GET (bb_gen_info_t, bb_gen_infos, i));
  VARR_DESTROY (bb_gen_info_t, bb_gen_infos);
}

static MIR_label_t get_mir_bb_label (LLVMBasicBlockRef bb) {
  struct bb_gen_info temp_bb_gen_info;
  bb_gen_info_t bb_gen_info;

  temp_bb_gen_info.bb = bb;
  if (HTAB_DO (bb_gen_info_t, bb_gen_info_tab, &temp_bb_gen_info, HTAB_FIND, bb_gen_info))
    return bb_gen_info->label;
  bb_gen_info = malloc (sizeof (struct bb_gen_info));
  VARR_PUSH (bb_gen_info_t, bb_gen_infos, bb_gen_info);
  bb_gen_info->bb = bb;
  bb_gen_info->label = MIR_new_label (context);
  bb_gen_info->last = NULL;
  DLIST_INIT (out_edge_t, bb_gen_info->out_edges);
  HTAB_DO (bb_gen_info_t, bb_gen_info_tab, bb_gen_info, HTAB_INSERT, bb_gen_info);
  return bb_gen_info->label;
}

static MIR_insn_t update_last_bb_insn (LLVMBasicBlockRef bb, MIR_insn_t insn) {
  struct bb_gen_info temp_bb_gen_info;
  bb_gen_info_t bb_gen_info;
  int res;

  temp_bb_gen_info.bb = bb;
  res = HTAB_DO (bb_gen_info_t, bb_gen_info_tab, &temp_bb_gen_info, HTAB_FIND, bb_gen_info);
  assert (res);
  if (insn != NULL) bb_gen_info->last = insn;
  return bb_gen_info->last;
}

static out_edge_t get_out_edge (bb_gen_info_t bb_gen_info, LLVMBasicBlockRef dest_bb) {
  out_edge_t e;

  for (e = DLIST_HEAD (out_edge_t, bb_gen_info->out_edges); e != NULL;
       e = DLIST_NEXT (out_edge_t, e))
    if (e->bb_dest == dest_bb) break;
  if (e == NULL) {
    e = malloc (sizeof (struct out_edge));
    e->bb_dest = dest_bb;
    e->br_insns = NULL;
    DLIST_INIT (edge_phi_op_eval_t, e->op_evals);
    DLIST_APPEND (out_edge_t, bb_gen_info->out_edges, e);
  }
  return e;
}

static void add_phi_op_eval (LLVMBasicBlockRef from_bb, LLVMBasicBlockRef phi_bb,
                             DLIST (MIR_insn_t) insns) {
  edge_phi_op_eval_t pi = malloc (sizeof (struct edge_phi_op_eval));
  struct bb_gen_info temp_bb_gen_info;
  bb_gen_info_t bb_gen_info;
  out_edge_t e;
  int res;

  get_mir_bb_label (from_bb); /* create bb_gen_info if it is not created yet. */
  temp_bb_gen_info.bb = from_bb;
  res = HTAB_DO (bb_gen_info_t, bb_gen_info_tab, &temp_bb_gen_info, HTAB_FIND, bb_gen_info);
  assert (res);
  e = get_out_edge (bb_gen_info, phi_bb);
  pi->insns = insns;
  DLIST_APPEND (edge_phi_op_eval_t, e->op_evals, pi);
}

static void add_bb_dest (LLVMBasicBlockRef bb, LLVMBasicBlockRef dest_bb, MIR_insn_t mir_insn) {
  struct bb_gen_info temp_bb_gen_info;
  bb_gen_info_t bb_gen_info;
  out_edge_t e;
  int res;

  get_mir_bb_label (bb); /* create bb_gen_info if it is not created yet. */
  temp_bb_gen_info.bb = bb;
  res = HTAB_DO (bb_gen_info_t, bb_gen_info_tab, &temp_bb_gen_info, HTAB_FIND, bb_gen_info);
  assert (res);
  e = get_out_edge (bb_gen_info, dest_bb);
  if (e->br_insns == NULL) VARR_CREATE (MIR_insn_t, e->br_insns, 16);
  VARR_PUSH (MIR_insn_t, e->br_insns, mir_insn);
}

struct item {
  MIR_name_t name;
  MIR_item_t item;
};

typedef struct item item_t;
DEF_HTAB (item_t);
static HTAB (item_t) * item_tab;

static int item_eq (item_t item1, item_t item2, void *arg) {
  return strcmp (item1.name, item2.name) == 0;
}
static htab_hash_t item_hash (item_t item, void *arg) {
  return mir_hash (item.name, strlen (item.name), 424);
}

static MIR_item_t find_item (MIR_name_t name) {
  struct item temp_item;

  temp_item.name = name;
  if (!HTAB_DO (item_t, item_tab, temp_item, HTAB_FIND, temp_item)) return NULL;
  return temp_item.item;
}

static void add_item (MIR_item_t item) {
  struct item temp_item;

  temp_item.name = MIR_item_name (context, item);
  temp_item.item = item;
  if (HTAB_DO (item_t, item_tab, temp_item, HTAB_INSERT, temp_item))
    assert ("repeated item inclusion");
}

struct expr_res {
  LLVMValueRef expr; /* insn/expr in the curent func */
  MIR_reg_t reg;     /* MIR register for insn result */
};

typedef struct expr_res expr_res_t;
DEF_HTAB (expr_res_t);
static HTAB (expr_res_t) * expr_res_tab;

static int expr_res_eq (expr_res_t expr_res1, expr_res_t expr_res2, void *arg) {
  return expr_res1.expr == expr_res2.expr;
}
static htab_hash_t expr_res_hash (expr_res_t expr_res, void *arg) {
  return mir_hash_finish (mir_hash_step (mir_hash_init (0x42), (uint64_t) expr_res.expr));
}

static void add_mir_reg_to_table (LLVMValueRef expr, MIR_reg_t reg) {
  expr_res_t temp_res;
  int already_in_table;

  temp_res.expr = expr;
  temp_res.reg = reg;
  already_in_table = HTAB_DO (expr_res_t, expr_res_tab, temp_res, HTAB_INSERT, temp_res);
  assert (!already_in_table);
}

static MIR_type_t mir_reg_type (MIR_type_t mir_type) {
  if (mir_type == MIR_T_P || mir_type == MIR_T_I8 || mir_type == MIR_T_I16 || mir_type == MIR_T_I32
      || mir_type == MIR_T_U8 || mir_type == MIR_T_U16 || mir_type == MIR_T_U32)
    return MIR_T_I64;
  return mir_type;
}
static MIR_reg_t get_expr_res_reg (LLVMValueRef expr, MIR_type_t mir_type) {
  expr_res_t temp_res, expr_res;
  char buf[30];

  temp_res.expr = expr;
  if (HTAB_DO (expr_res_t, expr_res_tab, temp_res, HTAB_FIND, expr_res)) return expr_res.reg;
  sprintf (buf, "%%%u", curr_mir_func_reg_num++);
  temp_res.reg = MIR_new_func_reg (context, curr_mir_func->u.func, mir_reg_type (mir_type), buf);
  add_mir_reg_to_table (expr, temp_res.reg);
  return temp_res.reg;
}

static const char *get_func_name (LLVMValueRef op) {
  return LLVMGetValueKind (op) == LLVMFunctionValueKind ? LLVMGetValueName (op) : NULL;
}

static int intrinsic_p (const char *name) { return strncmp (name, "llvm.", strlen ("llvm.")) == 0; }

static int ignored_intrinsic_p (const char *name) {
  static const char *ignored_prefix[] = {"llvm.lifetime.", "llvm.dbg."};

  for (unsigned i = 0; i < sizeof (ignored_prefix) / sizeof (char *); i++)
    if (strncmp (name, ignored_prefix[i], strlen (ignored_prefix[i])) == 0) return TRUE;
  return FALSE;
}

static int llvm_double_type_kind_p (LLVMTypeKind type_id) { return type_id == LLVMDoubleTypeKind; }

static int llvm_long_double_type_kind_p (LLVMTypeKind type_id) {
  return type_id == LLVMX86_FP80TypeKind;
}

static int llvm_fp_type_kind_p (LLVMTypeKind type_id) {
  return (type_id == LLVMFloatTypeKind || llvm_double_type_kind_p (type_id)
          || llvm_long_double_type_kind_p (type_id));
}

static MIR_type_t mir_var_type (MIR_type_t type) {
  return type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD ? type : MIR_T_I64;
}

static MIR_insn_code_t mir_mov_code (MIR_type_t type) {
  return (type == MIR_T_F ? MIR_FMOV
                          : type == MIR_T_D ? MIR_DMOV : type == MIR_T_LD ? MIR_LDMOV : MIR_MOV);
}

static MIR_type_t mir_type_of_type_id (LLVMTypeKind type_id) {
  return (type_id == LLVMFloatTypeKind
            ? MIR_T_F
            : llvm_double_type_kind_p (type_id)
                ? MIR_T_D
                : llvm_long_double_type_kind_p (type_id) ? MIR_T_LD : MIR_T_I64);
}

static int get_hex (int ch) {
  uint8_t v;
  if ('0' <= ch && ch <= '9')
    return ch - '0';
  else if ('A' <= ch && ch <= 'F')
    return ch - 'A' + 10;
  else
    error ("wrong long double constant");
}

static long double get_long_double_value (LLVMValueRef op) {  // ???
  LLVMBool lose;
  size_t i, j;
  uint8_t t;
  union {
    long double ld;
    uint8_t a[16];
  } u;
  const char *prefix = "x86_fp80 0xK";
  char *str = LLVMPrintValueToString (op);

  if (strncmp (str, prefix, strlen (prefix)) != 0) error ("unsupported long double constant");
  for (j = 0, i = strlen (prefix); str[i] != '\0' && j < 16; i += 2)
    u.a[j++] = (get_hex (str[i]) << 4) | get_hex (str[i + 1]);
  if (str[i] != '\0') error ("wrong long double constant");
  if (LLVMByteOrder (TD) == LLVMLittleEndian)
    for (i = 0, j--; i < j; i++, j--) {
      t = u.a[i];
      u.a[i] = u.a[j];
      u.a[j] = t;
    }
  LLVMDisposeMessage (str);
  return u.ld;
}

static void process_expr (LLVMOpcode opcode, LLVMValueRef expr);

static MIR_item_t get_item (MIR_name_t name) {
  MIR_item_t item = find_item (name);

  if (item != NULL) return item;
  item = MIR_new_forward (context, name);
  DLIST_REMOVE (MIR_item_t, curr_mir_module->items, item);
  DLIST_PREPEND (MIR_item_t, curr_mir_module->items, item);
  return item;
}

static MIR_op_t get_mir_op (LLVMValueRef op, MIR_type_t mir_type) {
  unsigned bw;
  LLVMTypeRef type;
  LLVMTypeKind type_id;
  LLVMValueKind op_id;
  LLVMBool lose;

  for (;;) {
    op_id = LLVMGetValueKind (op);
    if (op_id == LLVMInstructionValueKind || op_id == LLVMArgumentValueKind)
      return MIR_new_reg_op (context, get_expr_res_reg (op, mir_type));
    if (op_id == LLVMConstantIntValueKind) {
      type = LLVMTypeOf (op);
      bw = LLVMGetIntTypeWidth (type); /* 1 is for Bool */
      return MIR_new_int_op (context, bw == 1 ? LLVMConstIntGetZExtValue (op)
                                              : LLVMConstIntGetSExtValue (op));
    }
    if (op_id == LLVMConstantFPValueKind) {
      type = LLVMTypeOf (op);
      type_id = LLVMGetTypeKind (type);
      if (type_id == LLVMFloatTypeKind)
        return MIR_new_float_op (context, (float) LLVMConstRealGetDouble (op, &lose));
      if (llvm_double_type_kind_p (type_id))
        return MIR_new_double_op (context, LLVMConstRealGetDouble (op, &lose));
      if (llvm_long_double_type_kind_p (type_id))
        return MIR_new_ldouble_op (context, get_long_double_value (op));
      error ("wrong float constant");
    }
    if (op_id == LLVMConstantPointerNullValueKind) return MIR_new_int_op (context, 0);
    if (op_id == LLVMFunctionValueKind) {
      MIR_item_t item = get_item (LLVMGetValueName (op));

      return MIR_new_ref_op (context, item);
    }
    if (op_id == LLVMGlobalVariableValueKind) {
      MIR_item_t item = get_item (LLVMGetValueName (op));

      return MIR_new_ref_op (context, item);
    }
    if (op_id == LLVMConstantVectorValueKind) error ("vector constant is not implemented yet");
    if (op_id == LLVMUndefValueValueKind) return MIR_new_int_op (context, 0);
    assert (op_id == LLVMConstantExprValueKind);
    process_expr (LLVMGetConstOpcode (op), op);
    return MIR_new_reg_op (context, get_expr_res_reg (op, mir_type));
  }
}

static MIR_insn_code_t get_mir_expr_code (LLVMOpcode opcode, MIR_type_t mir_type) {
  switch (opcode) {
  case LLVMAdd: return mir_type != MIR_T_I64 ? MIR_ADDS : MIR_ADD;
  case LLVMSub: return mir_type != MIR_T_I64 ? MIR_SUBS : MIR_SUB;
  case LLVMMul: return mir_type != MIR_T_I64 ? MIR_MULS : MIR_MUL;
  case LLVMUDiv: return mir_type != MIR_T_I64 ? MIR_UDIVS : MIR_UDIV;
  case LLVMSDiv: return mir_type != MIR_T_I64 ? MIR_DIVS : MIR_DIV;
  case LLVMURem: return mir_type != MIR_T_I64 ? MIR_UMODS : MIR_UMOD;
  case LLVMSRem: return mir_type != MIR_T_I64 ? MIR_MODS : MIR_MOD;
  case LLVMAnd: return mir_type != MIR_T_I64 ? MIR_ANDS : MIR_AND;
  case LLVMOr: return mir_type != MIR_T_I64 ? MIR_ORS : MIR_OR;
  case LLVMXor: return mir_type != MIR_T_I64 ? MIR_XORS : MIR_XOR;
  case LLVMShl: return mir_type != MIR_T_I64 ? MIR_LSHS : MIR_LSH;
  case LLVMLShr: return mir_type != MIR_T_I64 ? MIR_URSHS : MIR_URSH;
  case LLVMAShr: return mir_type != MIR_T_I64 ? MIR_RSHS : MIR_RSH;
  case LLVMFAdd: return mir_type == MIR_T_F ? MIR_FADD : mir_type == MIR_T_D ? MIR_DADD : MIR_LDADD;
  case LLVMFSub: return mir_type == MIR_T_F ? MIR_FSUB : mir_type == MIR_T_D ? MIR_DSUB : MIR_LDSUB;
  case LLVMFMul: return mir_type == MIR_T_F ? MIR_FMUL : mir_type == MIR_T_D ? MIR_DMUL : MIR_LDMUL;
  case LLVMFDiv:
    return mir_type == MIR_T_F ? MIR_FDIV : mir_type == MIR_T_D ? MIR_DDIV : MIR_LDDIV;
    // ??? case LLVMFRem: return short_p ? MIR_FMOD : MIR_DMOD;
  default: assert (FALSE);
  }
}

static MIR_op_t extend_op (int unsigned_p, unsigned bw, MIR_reg_t res_reg, MIR_op_t op) {
  MIR_op_t res_op;
  MIR_insn_code_t ext_insn_code, sh_insn_code;
  int sh = 0;

  assert (bw <= 32);
  res_op = MIR_new_reg_op (context, res_reg);
  ext_insn_code = (unsigned_p ? (bw <= 8 ? MIR_UEXT8 : bw <= 16 ? MIR_UEXT16 : MIR_UEXT32)
                              : (bw <= 8 ? MIR_EXT8 : bw <= 16 ? MIR_EXT16 : MIR_EXT32));
  sh_insn_code = unsigned_p ? MIR_URSH : MIR_RSH;
  if (bw < 8) {
    sh = 8 - bw;
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context, MIR_LSHS, res_op, op, MIR_new_int_op (context, sh)));
    op = res_op;
  } else if (8 < bw && bw < 16) {
    sh = 16 - bw;
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context, MIR_LSHS, res_op, op, MIR_new_int_op (context, sh)));
    op = res_op;
  } else if (16 < bw && bw < 32) {
    sh = 32 - bw;
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context, MIR_LSHS, res_op, op, MIR_new_int_op (context, sh)));
    op = res_op;
  }
  MIR_append_insn (context, curr_mir_func, MIR_new_insn (context, ext_insn_code, res_op, op));
  if (sh != 0) {
    if (bw > 16) sh_insn_code = unsigned_p ? MIR_URSH : MIR_RSH;
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context, sh_insn_code, res_op, res_op,
                                   MIR_new_int_op (context, sh)));
  }
  return res_op;
}

static void gen_bin_op (LLVMOpcode opcode, LLVMValueRef expr, int int_p) {
  LLVMValueRef op0 = LLVMGetOperand (expr, 0), op1 = LLVMGetOperand (expr, 1);
  LLVMTypeRef type = LLVMTypeOf (op0);
  LLVMTypeKind type_id = LLVMGetTypeKind (type);
  unsigned bw;
  MIR_reg_t res_reg;
  MIR_op_t mir_op0, mir_op1;
  MIR_type_t mir_type;

  if (int_p && type_id == LLVMIntegerTypeKind) {
    if ((bw = LLVMGetIntTypeWidth (type)) > 64)
      error ("We don't support LLVM integer types > 64-bits");
    mir_op0 = get_mir_op (op0, MIR_T_I64);
    mir_op1 = get_mir_op (op1, MIR_T_I64);
    res_reg = get_expr_res_reg (expr, MIR_T_I64);
    if (bw < 32) {
      int unsigned_p = opcode == LLVMLShr || opcode == LLVMUDiv || opcode == LLVMURem;
      mir_op0 = extend_op (unsigned_p, bw, mir_int_temp_reg, mir_op0);
      mir_op1 = extend_op (unsigned_p, bw, res_reg, mir_op1);
    }
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context,
                                   get_mir_expr_code (opcode, bw <= 32 ? MIR_T_I32 : MIR_T_I64),
                                   MIR_new_reg_op (context, res_reg), mir_op0, mir_op1));
  } else if (!int_p && llvm_fp_type_kind_p (type_id)) {
    mir_type = mir_type_of_type_id (type_id);
    mir_op0 = get_mir_op (op0, mir_type);
    mir_op1 = get_mir_op (op1, mir_type);
    res_reg = get_expr_res_reg (expr, mir_type);
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context, get_mir_expr_code (opcode, mir_type),
                                   MIR_new_reg_op (context, res_reg), mir_op0, mir_op1));
  } else if (type_id == LLVMVectorTypeKind) {
    error ("vectors are not implemented: don't use autovectorization or complex");
  } else {
    error ("invalid combination of operand types for binary operand expr");
  }
}

static MIR_insn_code_t get_mir_expr_icmp_code (LLVMIntPredicate pred, int short_p) {
  switch (pred) {
  case LLVMIntEQ: return short_p ? MIR_EQS : MIR_EQ;
  case LLVMIntNE: return short_p ? MIR_NES : MIR_NE;
  case LLVMIntUGT: return short_p ? MIR_UGTS : MIR_UGT;
  case LLVMIntUGE: return short_p ? MIR_UGES : MIR_UGE;
  case LLVMIntULT: return short_p ? MIR_ULTS : MIR_ULT;
  case LLVMIntULE: return short_p ? MIR_ULES : MIR_ULE;
  case LLVMIntSGT: return short_p ? MIR_GTS : MIR_GT;
  case LLVMIntSGE: return short_p ? MIR_GES : MIR_GE;
  case LLVMIntSLT: return short_p ? MIR_LTS : MIR_LT;
  case LLVMIntSLE: return short_p ? MIR_LES : MIR_LE;
  default: error ("wrong integer predicate");
  }
}

static void gen_icmp_op (LLVMValueRef expr) {
  LLVMValueRef op0 = LLVMGetOperand (expr, 0), op1 = LLVMGetOperand (expr, 1);
  LLVMTypeRef type = LLVMTypeOf (op0);
  LLVMTypeKind type_id = LLVMGetTypeKind (type);
  LLVMIntPredicate pred = LLVMGetICmpPredicate (expr);
  unsigned bw;
  MIR_reg_t res_reg;
  MIR_op_t mir_op0, mir_op1;

  if (type_id == LLVMIntegerTypeKind || type_id == LLVMPointerTypeKind) {
    if (type_id == LLVMPointerTypeKind)
#if MIR_PTR32
      bw = 32;
#else
      bw = 64;
#endif
    else if ((bw = LLVMGetIntTypeWidth (type)) > 64)
      error ("We don't support LLVM integer types > 64-bits");
    mir_op0 = get_mir_op (op0, MIR_T_I64);
    mir_op1 = get_mir_op (op1, MIR_T_I64);
    res_reg = get_expr_res_reg (expr, MIR_T_I64);
    if (bw < 32) {
      int unsigned_p
        = pred == LLVMIntUGT || pred == LLVMIntUGE || pred == LLVMIntULT || pred == LLVMIntULE;

      mir_op0 = extend_op (unsigned_p, bw, mir_int_temp_reg, mir_op0);
      mir_op1 = extend_op (unsigned_p, bw, res_reg, mir_op1);
    }
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context, get_mir_expr_icmp_code (pred, bw <= 32),
                                   MIR_new_reg_op (context, res_reg), mir_op0, mir_op1));
  } else if (type_id == LLVMPointerTypeKind) {
  } else if (type_id == LLVMVectorTypeKind) {
    error ("vectors are not implemented: don't use autovectorization");
  } else {
    error ("invalid combination of operand types for int compare expr");
  }
}

static MIR_insn_code_t get_mir_expr_fcmp_code (LLVMRealPredicate pred, MIR_type_t type,
                                               int *move_val) {
  switch (pred) {  // ordered and unordered ???
  case LLVMRealUEQ:
  case LLVMRealOEQ: return type == MIR_T_F ? MIR_FEQ : type == MIR_T_D ? MIR_DEQ : MIR_LDEQ;
  case LLVMRealUNE:
  case LLVMRealONE: return type == MIR_T_F ? MIR_FNE : type == MIR_T_D ? MIR_DNE : MIR_LDNE;
  case LLVMRealUGT:
  case LLVMRealOGT: return type == MIR_T_F ? MIR_FGT : type == MIR_T_D ? MIR_DGT : MIR_LDGT;
  case LLVMRealUGE:
  case LLVMRealOGE: return type == MIR_T_F ? MIR_FGE : type == MIR_T_D ? MIR_DGE : MIR_LDGE;
  case LLVMRealULT:
  case LLVMRealOLT: return type == MIR_T_F ? MIR_FLT : type == MIR_T_D ? MIR_DLT : MIR_LDLT;
  case LLVMRealULE:
  case LLVMRealOLE: return type == MIR_T_F ? MIR_FLE : type == MIR_T_D ? MIR_DLE : MIR_LDLE;
  case LLVMRealPredicateFalse: *move_val = 0; return MIR_MOV;
  case LLVMRealPredicateTrue: *move_val = 1; return MIR_MOV;
  default: error ("unsupported real predicate");
  }
}

static void gen_fcmp_op (LLVMValueRef expr) {
  LLVMValueRef op0 = LLVMGetOperand (expr, 0), op1 = LLVMGetOperand (expr, 1);
  LLVMTypeRef type = LLVMTypeOf (op0);
  LLVMTypeKind type_id = LLVMGetTypeKind (type);
  LLVMRealPredicate pred = LLVMGetFCmpPredicate (expr);
  MIR_reg_t res_reg;
  MIR_op_t mir_op0, mir_op1;
  MIR_type_t mir_type;
  MIR_insn_code_t mir_insn_code;
  MIR_insn_t mir_insn;
  int move_val;

  if (llvm_fp_type_kind_p (type_id)) {
    mir_type = mir_type_of_type_id (type_id);
    mir_op0 = get_mir_op (op0, mir_type);
    mir_op1 = get_mir_op (op1, mir_type);
    res_reg = get_expr_res_reg (expr, mir_type);
    mir_insn_code = get_mir_expr_fcmp_code (pred, mir_type, &move_val);
    mir_insn = (mir_insn_code == MIR_MOV
                  ? MIR_new_insn (context, MIR_MOV, MIR_new_reg_op (context, res_reg),
                                  MIR_new_int_op (context, move_val))
                  : MIR_new_insn (context, mir_insn_code, MIR_new_reg_op (context, res_reg),
                                  mir_op0, mir_op1));
    MIR_append_insn (context, curr_mir_func, mir_insn);
  } else if (type_id == LLVMVectorTypeKind) {
    error ("vectors are not implemented: don't use autovectorization");
  } else {
    error ("invalid combination of operand types for real compare expr");
  }
}

static MIR_type_t get_mir_type (LLVMTypeRef type) {
  switch ((LLVMTypeKind) LLVMGetTypeKind (type)) {
  case LLVMIntegerTypeKind: {
    unsigned bw = LLVMGetIntTypeWidth (type);

    if (bw <= 8) return MIR_T_I8;
    if (bw <= 16) return MIR_T_I16;
    if (bw <= 32) return MIR_T_I32;
    if (bw <= 64) return MIR_T_I64;
    error ("integer type > 64-bits");
  }
  case LLVMFloatTypeKind: return MIR_T_F;
  case LLVMDoubleTypeKind: return MIR_T_D;
  case LLVMX86_FP80TypeKind: return MIR_T_LD;
  case LLVMPointerTypeKind:
  case LLVMFunctionTypeKind:
  case LLVMLabelTypeKind: return MIR_T_P;
  case LLVMVectorTypeKind: error ("vectors are not implemented: don't use autovectorization");
  default: LLVMDumpType (type); error (" type unrepresentable by MIR types");
  }
}

static MIR_reg_t force_ptr_to_reg (MIR_op_t mir_op) {
  if (mir_op.mode == MIR_OP_REG) return mir_op.u.reg;
  MIR_append_insn (context, curr_mir_func,
                   MIR_new_insn (context, MIR_MOV, MIR_new_reg_op (context, mir_int_temp_reg),
                                 mir_op));
  return mir_int_temp_reg;
}

static VARR (MIR_var_t) * mir_vars;

DEF_VARR (MIR_op_t)
static VARR (MIR_op_t) * mir_ops;

DEF_VARR (LLVMTypeRef)
static VARR (LLVMTypeRef) * types;

static MIR_item_t get_proto (LLVMTypeRef ftype, unsigned *proto_num) {
  unsigned nparams = LLVMCountParamTypes (ftype);
  LLVMTypeRef ret_type = LLVMGetReturnType (ftype);
  MIR_item_t proto;
  MIR_var_t var;
  MIR_type_t mir_type;
  int nres;
  char buf[30];

  VARR_EXPAND (LLVMTypeRef, types, nparams);
  LLVMGetParamTypes (ftype, VARR_ADDR (LLVMTypeRef, types));
  VARR_TRUNC (MIR_var_t, mir_vars, 0);
  for (unsigned i = 0; i < nparams; i++) {
    var.name = "p";
    var.type = get_mir_type (VARR_ADDR (LLVMTypeRef, types)[i]);
    VARR_PUSH (MIR_var_t, mir_vars, var);
  }
  sprintf (buf, "$p%d", *proto_num);
  (*proto_num)++;
  if ((nres = LLVMGetTypeKind (ret_type) != LLVMVoidTypeKind)) mir_type = get_mir_type (ret_type);
  proto
    = ((LLVMIsFunctionVarArg (ftype) ? MIR_new_vararg_proto_arr
                                     : MIR_new_proto_arr) (context, buf, nres, &mir_type, nparams,
                                                           VARR_ADDR (MIR_var_t, mir_vars)));
  DLIST_REMOVE (MIR_item_t, curr_mir_module->items, proto);
  DLIST_INSERT_BEFORE (MIR_item_t, curr_mir_module->items,
                       DLIST_TAIL (MIR_item_t, curr_mir_module->items), proto);
  return proto;
}

struct set_insn {
  unsigned check;
  MIR_insn_t insn;
};

typedef struct set_insn set_insn_t;

DEF_VARR (set_insn_t);
static VARR (set_insn_t) * set_insns;
static unsigned curr_set_insn_check, curr_phi_loop_reg_num;

static void update_set_insn (MIR_insn_t insn) {
  MIR_op_t op = insn->ops[0];
  MIR_reg_t reg;
  set_insn_t set_insn;

  assert (op.mode == MIR_OP_REG);
  reg = op.u.reg;
  set_insn.check = 0;
  while (VARR_LENGTH (set_insn_t, set_insns) <= reg) VARR_PUSH (set_insn_t, set_insns, set_insn);
  set_insn.check = curr_set_insn_check;
  set_insn.insn = insn;
  VARR_SET (set_insn_t, set_insns, reg, set_insn);
}

static MIR_insn_t get_set_insn (MIR_reg_t reg) {
  set_insn_t set_insn;

  if (VARR_LENGTH (set_insn_t, set_insns) <= reg) return NULL;
  set_insn = VARR_GET (set_insn_t, set_insns, reg);
  return set_insn.check != curr_set_insn_check ? NULL : set_insn.insn;
}

static void generate_edge_phi_op_eval (bb_gen_info_t bi) {
  MIR_insn_code_t insn_code;
  MIR_insn_t insn, prev_insn, set_insn, before, after, first = get_mir_bb_label (bi->bb);
  MIR_insn_t br_insn, new_insn, last_bb_insn = update_last_bb_insn (bi->bb, NULL);
  MIR_op_t op, res;
  MIR_reg_t temp_reg;
  MIR_label_t skip_label, new_bb_label;
  int out_p;
  char name[30];

  assert (first != NULL);
  for (out_edge_t e = DLIST_HEAD (out_edge_t, bi->out_edges); e != NULL;
       e = DLIST_NEXT (out_edge_t, e)) {
    if (e->br_insns == NULL) continue;
    for (size_t i = 0; i < VARR_LENGTH (MIR_insn_t, e->br_insns); i++) {
      before = after = NULL;
      br_insn = VARR_GET (MIR_insn_t, e->br_insns, i);
      if (br_insn == NULL)
        after = last_bb_insn == NULL ? first : last_bb_insn;
      else
        before = br_insn;
      curr_set_insn_check++;
      for (edge_phi_op_eval_t op_eval = DLIST_HEAD (edge_phi_op_eval_t, e->op_evals);
           op_eval != NULL; op_eval = DLIST_NEXT (edge_phi_op_eval_t, op_eval)) {
        DLIST (MIR_insn_t) insns;

        if (i == VARR_LENGTH (MIR_insn_t, e->br_insns) - 1) { /* last -- use original insns */
          insns = op_eval->insns;
        } else { /* non last -- use copy insns: */
          DLIST_INIT (MIR_insn_t, insns);
          for (insn = DLIST_HEAD (MIR_insn_t, op_eval->insns); insn != NULL;
               insn = DLIST_NEXT (MIR_insn_t, insn)) {
            new_insn = MIR_copy_insn (context, insn);
            DLIST_APPEND (MIR_insn_t, insns, new_insn);
          }
        }
        for (insn = DLIST_HEAD (MIR_insn_t, insns); insn != NULL;
             insn = DLIST_NEXT (MIR_insn_t, insn)) {
          for (size_t i = 0; i < MIR_insn_nops (context, insn); i++) {
            MIR_op_mode_t op_mode = MIR_insn_op_mode (context, insn, i, &out_p);
            MIR_type_t type
              = (op_mode == MIR_OP_FLOAT
                   ? MIR_T_F
                   : op_mode == MIR_OP_DOUBLE ? MIR_T_D
                                              : op_mode == MIR_OP_LDOUBLE ? MIR_T_LD : MIR_T_I64);

            if (out_p) continue;
            op = insn->ops[i];
            if (op.mode == MIR_OP_REG && (set_insn = get_set_insn (op.u.reg)) != NULL) {
              /* Removing loop: a = ... (;) ... = a;  =>  temp = a; a = ... (;) ... = temp; */
              res = set_insn->ops[0];
              assert (res.mode == MIR_OP_REG && res.u.reg == op.u.reg);
              sprintf (name, "%%phi_loop%u", curr_phi_loop_reg_num++);
              temp_reg = MIR_new_func_reg (context, curr_mir_func->u.func, type, name);
              insn_code = mir_mov_code (type);
              MIR_insert_insn_before (context, curr_mir_func, set_insn,
                                      MIR_new_insn (context, insn_code,
                                                    MIR_new_reg_op (context, temp_reg), res));
              insn->ops[i] = MIR_new_reg_op (context, temp_reg);
            }
          }
        }
        assert (DLIST_HEAD (MIR_insn_t, insns) != NULL);
        skip_label = new_bb_label = NULL;
        if (before != NULL && MIR_branch_code_p (before->code) && before->code != MIR_JMP) {
          /* BR label => JMP skip_label; new_bb_label: ... */
          skip_label = MIR_new_label (context);
          new_bb_label = MIR_new_label (context);
          MIR_insert_insn_before (context, curr_mir_func, before,
                                  MIR_new_insn (context, MIR_JMP,
                                                MIR_new_label_op (context, skip_label)));
          MIR_insert_insn_before (context, curr_mir_func, before, new_bb_label);
        }
        while ((insn = DLIST_HEAD (MIR_insn_t, insns)) != NULL) {
          DLIST_REMOVE (MIR_insn_t, insns, insn);
          if (after == NULL) {
            MIR_insert_insn_before (context, curr_mir_func, before, insn);
          } else {
            MIR_insert_insn_after (context, curr_mir_func, after, insn);
          }
          after = insn;
          prev_insn = insn;
        }
        update_set_insn (prev_insn); /* only the last insn sets up phi var */
        if (skip_label != NULL) {
          /* ... => JMP label; skip_label: BR new_bb_label; */
          assert (before->ops[0].mode == MIR_OP_LABEL);
          MIR_insert_insn_before (context, curr_mir_func, before,
                                  MIR_new_insn (context, MIR_JMP, before->ops[0]));
          MIR_insert_insn_before (context, curr_mir_func, before, skip_label);
          before->ops[0] = MIR_new_label_op (context, new_bb_label);
        }
      }
    }
  }
}

static void init_phi_generation (void) {
  curr_set_insn_check = curr_phi_loop_reg_num = 0;
  VARR_CREATE (set_insn_t, set_insns, 0);
}

static void finish_phi_generation (void) { VARR_DESTROY (set_insn_t, set_insns); }

static LLVMValueRef skip_pointer_bitcast (LLVMValueRef op) {
  LLVMTypeRef type;

  while (LLVMGetValueKind (op) != LLVMGlobalVariableValueKind
         && LLVMGetConstOpcode (op) == LLVMBitCast) {
    LLVMTypeRef type = LLVMTypeOf (op);

    assert (LLVMGetTypeKind (type) == LLVMPointerTypeKind);
    op = LLVMGetOperand (op, 0);
  }
  return op;
}

static MIR_item_t gen_ref_data (LLVMValueRef op, const char *name) {
  LLVMValueRef op0, op1;
  MIR_op_t mir_op0;
  LLVMTypeRef type0;
  LLVMTypeKind type_id;
  unsigned long el_size, index, offset = 0;

  op = skip_pointer_bitcast (op);
  if (LLVMGetValueKind (op) == LLVMGlobalVariableValueKind) {
    mir_op0 = get_mir_op (op, MIR_T_P);
    assert (mir_op0.mode == MIR_OP_REF);
    return MIR_new_ref_data (context, name, mir_op0.u.ref, 0);
  }
  assert (LLVMGetConstOpcode (op) == LLVMGetElementPtr);
  op0 = LLVMGetOperand (op, 0);
  type0 = LLVMTypeOf (op0);
  type_id = LLVMGetTypeKind (type0);
  assert (type_id == LLVMPointerTypeKind);
  if (LLVMGetValueKind (op0) == LLVMConstantExprValueKind
      && LLVMGetConstOpcode (op0) == LLVMBitCast) {
    assert (LLVMGetNumOperands (op) == 2);
    op0 = LLVMGetOperand (op0, 0);
    op1 = LLVMGetOperand (op, 1);
    index = LLVMConstIntGetSExtValue (op1);
    type0 = LLVMGetElementType (type0);
    el_size = LLVMABISizeOfType (TD, type0);
    offset += index * el_size;
    mir_op0 = get_mir_op (op0, MIR_T_P);
    assert (mir_op0.mode == MIR_OP_REF);
    return MIR_new_ref_data (context, name, mir_op0.u.ref, offset);
  }
  mir_op0 = get_mir_op (op0, MIR_T_P);
  assert (mir_op0.mode == MIR_OP_REF);
  for (unsigned j = 1; j < LLVMGetNumOperands (op); j++) {
    op1 = LLVMGetOperand (op, j);
    if (type_id == LLVMStructTypeKind) {
      assert (LLVMGetValueKind (op1) == LLVMConstantIntValueKind);
      index = LLVMConstIntGetSExtValue (op1);
      offset += LLVMOffsetOfElement (TD, type0, index);
      type0 = LLVMStructGetTypeAtIndex (type0, index);
    } else {
      assert (LLVMGetValueKind (op1) == LLVMConstantIntValueKind);
      type0 = LLVMGetElementType (type0);
      el_size = LLVMABISizeOfType (TD, type0);
      index = LLVMConstIntGetSExtValue (op1);
      offset += index * el_size;
    }
    type_id = LLVMGetTypeKind (type0);
  }
  return MIR_new_ref_data (context, name, mir_op0.u.ref, offset);
}

DEF_VARR (char);
static VARR (char) * string;

static MIR_item_t gen_data_bss (LLVMTypeRef type, const char *name, LLVMValueRef init) {
  MIR_type_t mir_type;
  MIR_item_t first_item = NULL, item;
  LLVMTypeRef el_type;
  LLVMValueKind init_id = LLVMGetValueKind (init);
  LLVMValueRef op;
  LLVMBool lose;
  union {
    int8_t i8;
    int16_t i16;
    int32_t i32;
    int64_t i64;
    float f;
    double d;
    long double ld;
  } v;
  unsigned i, n;
  unsigned long size = LLVMABISizeOfType (TD, type);

  if (init == NULL || LLVMGetValueKind (init) == LLVMConstantAggregateZeroValueKind
      || LLVMGetValueKind (init) == LLVMConstantPointerNullValueKind) {
    first_item = MIR_new_bss (context, name, size);
  } else if (init_id == LLVMConstantIntValueKind || init_id == LLVMConstantFPValueKind) {
    switch (mir_type = get_mir_type (type)) {
    case MIR_T_I8: v.i8 = LLVMConstIntGetSExtValue (init); break;
    case MIR_T_I16: v.i16 = LLVMConstIntGetSExtValue (init); break;
    case MIR_T_I32: v.i32 = LLVMConstIntGetSExtValue (init); break;
    case MIR_T_I64: v.i64 = LLVMConstIntGetSExtValue (init); break;
    case MIR_T_F: v.f = (float) LLVMConstRealGetDouble (init, &lose); break;
    case MIR_T_D: v.d = LLVMConstRealGetDouble (init, &lose); break;
    case MIR_T_LD: v.ld = get_long_double_value (init); break;
    default: assert (FALSE);
    }
    first_item = MIR_new_data (context, name, mir_type, 1, &v);
  } else if (init_id == LLVMConstantDataArrayValueKind || init_id == LLVMConstantArrayValueKind) {
    int c, data_p = FALSE;

    if (0 && init_id == LLVMConstantDataArrayValueKind)
      type = LLVMGetElementType (type); /* Skip pointer */
    el_type = LLVMGetElementType (type);
    if (LLVMIsConstantString (init)) {
      VARR_TRUNC (char, string, 0);
      for (i = 0;;) {
        op = LLVMGetElementAsConstant (init, i);
        c = LLVMConstIntGetZExtValue (op);
        VARR_PUSH (char, string, c);
        i++;
        if (i >= LLVMGetArrayLength (type)) break;
        if (c == 0) data_p = TRUE;
      }
      first_item = (data_p ? MIR_new_data (context, name, MIR_T_I8, VARR_LENGTH (char, string),
                                           VARR_ADDR (char, string))
                           : MIR_new_string_data (context, name,
                                                  (MIR_str_t){VARR_LENGTH (char, string),
                                                              VARR_ADDR (char, string)}));
    } else {
      LLVMValueKind op_id;
      LLVMTypeKind el_type_id = LLVMGetTypeKind (el_type);
      size_t start, len;
      MIR_type_t mir_type = MIR_T_BOUND;

      VARR_TRUNC (char, string, 0);
      for (i = 0; i < LLVMGetArrayLength (type); i++) {
        op = init_id == LLVMConstantArrayValueKind ? LLVMGetOperand (init, i)
                                                   : LLVMGetElementAsConstant (init, i);
        if ((op_id = LLVMGetValueKind (op)) == LLVMGlobalVariableValueKind
            || op_id == LLVMConstantExprValueKind) {
          mir_type = MIR_T_BOUND;
          item = gen_ref_data (op, name);
          if (first_item == NULL) first_item = item;
          name = NULL;
          continue;
        }
        switch (el_type_id) {
        case LLVMIntegerTypeKind: {
          if ((n = LLVMGetIntTypeWidth (el_type)) > 64) error ("integer type > 64-bits");
          mir_type = n <= 8 ? MIR_T_I8 : n <= 16 ? MIR_T_I16 : n <= 32 ? MIR_T_I32 : MIR_T_I64;
          break;
        }
        case LLVMFloatTypeKind: mir_type = MIR_T_F; break;
        case LLVMDoubleTypeKind: mir_type = MIR_T_D; break;
        case LLVMX86_FP80TypeKind: mir_type = MIR_T_LD; break;
        case LLVMPointerTypeKind:
        case LLVMFunctionTypeKind:
        case LLVMLabelTypeKind: mir_type = MIR_T_P; break;
        case LLVMVectorTypeKind: error ("vectors are not implemented: don't use autovectorization");
        default:
          mir_type = MIR_T_BOUND;
          item = gen_data_bss (el_type, name, op);
          if (first_item == NULL) first_item = item;
          name = NULL;
          continue;
        }
        switch (mir_type) {
        case MIR_T_I8: v.i8 = LLVMConstIntGetSExtValue (op); break;
        case MIR_T_I16: v.i16 = LLVMConstIntGetSExtValue (op); break;
#if MIR_PTR32
        case MIR_T_P:
#endif
        case MIR_T_I32: v.i32 = LLVMConstIntGetSExtValue (op); break;
#if MIR_PTR64
        case MIR_T_P:
#endif
        case MIR_T_I64: v.i64 = LLVMConstIntGetSExtValue (op); break;
        case MIR_T_F: v.f = (float) LLVMConstRealGetDouble (op, &lose); break;
        case MIR_T_D: v.d = LLVMConstRealGetDouble (op, &lose); break;
        case MIR_T_LD: v.ld = get_long_double_value (op); break;
        default: assert (FALSE);
        }
        start = VARR_LENGTH (char, string);
        len = _MIR_type_size (context, mir_type);
        for (size_t k = 0; k < len; k++) VARR_PUSH (char, string, 0);
        memcpy (VARR_ADDR (char, string) + start, &v, len);
      }
      if (mir_type != MIR_T_BOUND)
        first_item = MIR_new_data (context, name, mir_type, LLVMGetArrayLength (type),
                                   VARR_ADDR (char, string));
    }
  } else if (init_id == LLVMConstantStructValueKind) {
    size_t len = 0, offset;

    n = LLVMCountStructElementTypes (type);
    for (i = 0; i < n; i++, name = NULL) {
      offset = LLVMOffsetOfElement (TD, type, i);
      if (offset > len) item = MIR_new_bss (context, name, offset - len);
      len = offset;
      el_type = LLVMStructGetTypeAtIndex (type, i);
      len += LLVMABISizeOfType (TD, el_type);
      op = LLVMGetOperand (init, i);
      item = gen_data_bss (el_type, name, op);
      if (first_item == NULL) first_item = item;
    }
    if (size > len) item = MIR_new_bss (context, name, size - len);
  } else if (init_id == LLVMGlobalVariableValueKind || init_id == LLVMConstantExprValueKind) {
    item = gen_ref_data (init, name);
    if (first_item == NULL) first_item = item;
  } else {
    assert (FALSE);
  }
  return first_item;
}

static MIR_reg_t mir_2nd_mem_addr_reg;

static MIR_reg_t get_2nd_mem_addr_reg (void) {
  if (mir_2nd_mem_addr_reg != 0) return mir_2nd_mem_addr_reg;
  mir_2nd_mem_addr_reg
    = MIR_new_func_reg (context, curr_mir_func->u.func, mir_reg_type (MIR_T_P), "$2nd_mem_addr");
  MIR_prepend_insn (context, curr_mir_func,
                    MIR_new_insn (context, MIR_ALLOCA,
                                  MIR_new_reg_op (context, mir_2nd_mem_addr_reg),
                                  MIR_new_int_op (context, 0)));
  return mir_2nd_mem_addr_reg;
}

static void process_expr (LLVMOpcode opcode, LLVMValueRef expr) {
  int ptr_size;
  MIR_op_t mir_op0, mir_op1, mir_op2;
  MIR_reg_t res_reg, ptr_reg, base_reg, index_reg;
  MIR_insn_code_t mir_insn_code;
  MIR_type_t mir_type;
  LLVMTypeRef type;
  LLVMTypeKind type_id;
  LLVMValueRef op0, op1, op2;
  unsigned long el_size;

  ptr_size = LLVMPointerSize (TD);
  switch (opcode) {
#if 0
  case LLVMFNeg:
    op0 = LLVMGetOperand (expr, 0);
    type = LLVMTypeOf (expr); type_id = LLVMGetTypeKind (type);
    assert (llvm_fp_type_kind_p (type_id));
    mir_type = get_mir_type (type);
    res_reg = get_expr_res_reg (expr, mir_type);
    mir_op0 = get_mir_op (op0, mir_type);
    MIR_append_insn (context, curr_mir_func,
		     MIR_new_insn (context, mir_type == MIR_T_F ? FNEG : mir_type == MIR_T_D ? DNEG : LDNEG,
				   MIR_new_reg_op (context, res_reg), mir_op0));
    break;
#endif
  case LLVMAdd:
  case LLVMSub:
  case LLVMMul:
  case LLVMUDiv:
  case LLVMSDiv:
  case LLVMURem:
  case LLVMSRem:
  case LLVMAnd:
  case LLVMOr:
  case LLVMXor: gen_bin_op (opcode, expr, TRUE); break;
  case LLVMFAdd:
  case LLVMFSub:
  case LLVMFMul:
  case LLVMFDiv:
  case LLVMFRem: /* ??? */
    gen_bin_op (opcode, expr, FALSE);
    break;

    /* Shifts */
  case LLVMShl:
  case LLVMLShr:
  case LLVMAShr: gen_bin_op (opcode, expr, TRUE); break;

  case LLVMGetElementPtr: {
    LLVMValueRef op1, op0 = LLVMGetOperand (expr, 0);
    unsigned long index, offset = 0;

    mir_op0 = get_mir_op (op0, MIR_T_I64);
    ptr_reg = force_ptr_to_reg (mir_op0);
    //	  ptr_reg = get_expr_res_reg (op0, MIR_T_I64);
    base_reg = 0;
    res_reg = get_expr_res_reg (expr, MIR_T_I64);
    type = LLVMTypeOf (op0);
    type_id = LLVMGetTypeKind (type);
    assert (type_id == LLVMPointerTypeKind);
    for (unsigned i = 1; i < LLVMGetNumOperands (expr); i++) {
      op1 = LLVMGetOperand (expr, i);
      if (type_id == LLVMStructTypeKind) {
        assert (LLVMGetValueKind (op1) == LLVMConstantIntValueKind);
        index = LLVMConstIntGetSExtValue (op1);
        offset += LLVMOffsetOfElement (TD, type, index);
        type = LLVMStructGetTypeAtIndex (type, index);
      } else {
        type = LLVMGetElementType (type);
        el_size = LLVMABISizeOfType (TD, type);
        if (LLVMGetValueKind (op1) == LLVMConstantIntValueKind) {
          index = LLVMConstIntGetSExtValue (op1);
          offset += index * el_size;
        } else if (base_reg == 0) {
          base_reg = res_reg;
          index_reg = get_expr_res_reg (op1, MIR_T_I64);
          MIR_append_insn (context, curr_mir_func,
                           MIR_new_insn (context, ptr_size == 4 ? MIR_MULS : MIR_MUL,
                                         MIR_new_reg_op (context, base_reg),
                                         MIR_new_reg_op (context, index_reg),
                                         MIR_new_int_op (context, el_size)));
        } else {
          index_reg = get_expr_res_reg (op1, MIR_T_I64);
          MIR_append_insn (context, curr_mir_func,
                           MIR_new_insn (context, ptr_size == 4 ? MIR_MULS : MIR_MUL,
                                         MIR_new_reg_op (context, mir_int_temp_reg),
                                         MIR_new_reg_op (context, index_reg),
                                         MIR_new_int_op (context, el_size)));
          MIR_append_insn (context, curr_mir_func,
                           MIR_new_insn (context, ptr_size == 4 ? MIR_ADDS : MIR_ADD,
                                         MIR_new_reg_op (context, base_reg),
                                         MIR_new_reg_op (context, base_reg),
                                         MIR_new_reg_op (context, mir_int_temp_reg)));
        }
      }
      type_id = LLVMGetTypeKind (type);
    }
    if (base_reg == 0) {
      if (offset == 0)
        MIR_append_insn (context, curr_mir_func,
                         MIR_new_insn (context, MIR_MOV, MIR_new_reg_op (context, res_reg),
                                       MIR_new_reg_op (context, ptr_reg)));
      else
        MIR_append_insn (context, curr_mir_func,
                         MIR_new_insn (context, ptr_size == 4 ? MIR_ADDS : MIR_ADD,
                                       MIR_new_reg_op (context, res_reg),
                                       MIR_new_reg_op (context, ptr_reg),
                                       MIR_new_int_op (context, offset)));
    } else {
      MIR_append_insn (context, curr_mir_func,
                       MIR_new_insn (context, ptr_size == 4 ? MIR_ADDS : MIR_ADD,
                                     MIR_new_reg_op (context, res_reg),
                                     MIR_new_reg_op (context, base_reg),
                                     MIR_new_reg_op (context, ptr_reg)));
      if (offset != 0)
        MIR_append_insn (context, curr_mir_func,
                         MIR_new_insn (context, ptr_size == 4 ? MIR_ADDS : MIR_ADD,
                                       MIR_new_reg_op (context, res_reg),
                                       MIR_new_reg_op (context, res_reg),
                                       MIR_new_int_op (context, offset)));
    }
    break;
  }
  case LLVMTrunc: /* no-op ??? */
    res_reg = get_expr_res_reg (expr, MIR_T_I64);
    op0 = LLVMGetOperand (expr, 0);
    mir_op0 = get_mir_op (op0, MIR_T_I64);
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context, MIR_MOV, MIR_new_reg_op (context, res_reg), mir_op0));
    break;
    /* Cast Operators */
  case LLVMZExt:
  case LLVMSExt: {
    unsigned bw;

    op0 = LLVMGetOperand (expr, 0);
    type = LLVMTypeOf (op0);
    bw = LLVMGetIntTypeWidth (type);
    assert (LLVMGetTypeKind (type) == LLVMIntegerTypeKind);
    res_reg = get_expr_res_reg (expr, MIR_T_I64);
    mir_op0 = get_mir_op (op0, MIR_T_I64);
    mir_type = get_mir_type (type);
    assert (mir_type == MIR_T_I8 || mir_type == MIR_T_I16 || mir_type == MIR_T_I32);
    if (opcode == LLVMSExt)
      mir_op0 = extend_op (FALSE, bw, res_reg, mir_op0);
    else
      mir_op0 = extend_op (TRUE, bw, res_reg, mir_op0);
    break;
  }
  case LLVMFPToUI:
  case LLVMFPToSI: {
    op0 = LLVMGetOperand (expr, 0);
    type = LLVMTypeOf (op0);
    type_id = LLVMGetTypeKind (type);
    if (!llvm_fp_type_kind_p (LLVMGetTypeKind (type)))
      error ("unsupported types for fptoui or fptous");
    mir_op0 = get_mir_op (op0, mir_type_of_type_id (type_id));
    res_reg = get_expr_res_reg (expr, MIR_T_I64);
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context,
                                   type_id == LLVMFloatTypeKind
                                     ? MIR_F2I
                                     : llvm_double_type_kind_p (type_id) ? MIR_D2I : MIR_LD2I,
                                   MIR_new_reg_op (context, res_reg), mir_op0));
    break;
  }
  case LLVMUIToFP:
  case LLVMSIToFP: {
    unsigned bw;

    op0 = LLVMGetOperand (expr, 0);
    type = LLVMTypeOf (op0);
    assert (LLVMGetTypeKind (type) == LLVMIntegerTypeKind);
    bw = LLVMGetIntTypeWidth (type);
    type = LLVMTypeOf (expr);
    type_id = LLVMGetTypeKind (type);
    mir_op0 = get_mir_op (op0, MIR_T_I64);
    res_reg = get_expr_res_reg (expr, mir_type_of_type_id (type_id));
    if (bw < 64) mir_op0 = extend_op (opcode == LLVMUIToFP, bw, mir_int_temp_reg, mir_op0);
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context,
                                   type_id == LLVMFloatTypeKind
                                     ? (opcode == LLVMUIToFP ? MIR_UI2F : MIR_I2F)
                                     : llvm_double_type_kind_p (type_id)
                                         ? (opcode == LLVMUIToFP ? MIR_UI2D : MIR_I2D)
                                         : (opcode == LLVMUIToFP ? MIR_UI2LD : MIR_I2LD),
                                   MIR_new_reg_op (context, res_reg), mir_op0));
    break;
  }
  case LLVMFPTrunc: {
    MIR_type_t from_type, to_type;

    op0 = LLVMGetOperand (expr, 0);
    type = LLVMTypeOf (op0);
    type_id = LLVMGetTypeKind (type);
    mir_insn_code = MIR_INSN_BOUND;
    if (llvm_long_double_type_kind_p (type_id)) {
      from_type = MIR_T_LD;
      if (LLVMGetTypeKind (LLVMTypeOf (expr)) == LLVMFloatTypeKind) {
        to_type = MIR_T_D;
        mir_insn_code = MIR_LD2D;
      } else if (llvm_double_type_kind_p (LLVMGetTypeKind (LLVMTypeOf (expr)))) {
        to_type = MIR_T_F;
        mir_insn_code = MIR_LD2F;
      }
    } else if (llvm_double_type_kind_p (type_id)) {
      from_type = MIR_T_D;
      if (LLVMGetTypeKind (LLVMTypeOf (expr)) == LLVMFloatTypeKind) {
        to_type = MIR_T_F;
        mir_insn_code = MIR_D2F;
      }
    }
    if (mir_insn_code == MIR_INSN_BOUND) error ("unsupported types for fptrunc");
    mir_op0 = get_mir_op (op0, from_type);
    res_reg = get_expr_res_reg (expr, to_type);
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context, mir_insn_code, MIR_new_reg_op (context, res_reg),
                                   mir_op0));
    break;
  }
  case LLVMFPExt: {
    MIR_type_t from_type, to_type;

    op0 = LLVMGetOperand (expr, 0);
    type = LLVMTypeOf (op0);
    type_id = LLVMGetTypeKind (type);
    mir_insn_code = MIR_INSN_BOUND;
    if (type_id == LLVMFloatTypeKind) {
      from_type = MIR_T_F;
      if (llvm_double_type_kind_p (LLVMGetTypeKind (LLVMTypeOf (expr)))) {
        to_type = MIR_T_D;
        mir_insn_code = MIR_F2D;
      } else if (llvm_long_double_type_kind_p (LLVMGetTypeKind (LLVMTypeOf (expr)))) {
        to_type = MIR_T_LD;
        mir_insn_code = MIR_F2LD;
      }
    } else if (llvm_double_type_kind_p (type_id)) {
      from_type = MIR_T_D;
      if (llvm_long_double_type_kind_p (LLVMGetTypeKind (LLVMTypeOf (expr)))) {
        to_type = MIR_T_LD;
        mir_insn_code = MIR_D2LD;
      }
    }
    if (mir_insn_code == MIR_INSN_BOUND) error ("unsupported types for fpext");
    mir_op0 = get_mir_op (op0, from_type);
    res_reg = get_expr_res_reg (expr, to_type);
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context, mir_insn_code, MIR_new_reg_op (context, res_reg),
                                   mir_op0));
    break;
  }
  case LLVMPtrToInt:
    op0 = LLVMGetOperand (expr, 0);
    type = LLVMTypeOf (expr);
    goto common_ptr_conv;
  case LLVMIntToPtr:
    op0 = LLVMGetOperand (expr, 0);
    type = LLVMTypeOf (op0);
  common_ptr_conv:
    assert (LLVMGetTypeKind (type) == LLVMIntegerTypeKind);
    mir_type = get_mir_type (type);
    mir_insn_code
      = (mir_type == MIR_T_I8
           ? MIR_UEXT8
           : mir_type == MIR_T_I16 ? MIR_UEXT16
                                   : mir_type == MIR_T_I64 && ptr_size == 8 ? MIR_MOV : MIR_UEXT32);
    res_reg = get_expr_res_reg (expr, MIR_T_I64);
    mir_op0 = get_mir_op (op0, MIR_T_I64);
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context, mir_insn_code, MIR_new_reg_op (context, res_reg),
                                   mir_op0));
    break;
  case LLVMBitCast: { /* it is mostly no-op */
    int float_op_p, float_res_p;
    MIR_type_t mir_op_type;

    op0 = LLVMGetOperand (expr, 0);
    type = LLVMTypeOf (op0);
    mir_op_type = get_mir_type (type);
    mir_insn_code = mir_mov_code (mir_op_type);
    mir_op0 = get_mir_op (op0, mir_op_type);
    float_op_p = mir_op_type == MIR_T_F || mir_op_type == MIR_T_D || mir_op_type == MIR_T_LD;
    type = LLVMTypeOf (expr);
    mir_type = get_mir_type (type);
    mir_type = mir_var_type (mir_type);
    float_res_p = mir_type == MIR_T_F || mir_type == MIR_T_D || mir_type == MIR_T_LD;
    res_reg = get_expr_res_reg (expr, mir_type);
    if (float_op_p == float_res_p) {
      MIR_append_insn (context, curr_mir_func,
                       MIR_new_insn (context, mir_insn_code, MIR_new_reg_op (context, res_reg),
                                     mir_op0));
    } else {
      MIR_reg_t addr = get_2nd_mem_addr_reg ();

      MIR_append_insn (context, curr_mir_func,
                       MIR_new_insn (context, mir_insn_code,
                                     MIR_new_mem_op (context, mir_op_type, 0, addr, 0, 1),
                                     mir_op0));
      MIR_append_insn (context, curr_mir_func,
                       MIR_new_insn (context, mir_mov_code (mir_type),
                                     MIR_new_reg_op (context, res_reg),
                                     MIR_new_mem_op (context, mir_type, 0, addr, 0, 1)));
    }
    break;
  }
  case LLVMAddrSpaceCast:
    error ("address spaces are not implemented");
    break;
    /* Other Operators */
  case LLVMICmp: gen_icmp_op (expr); break;
  case LLVMFCmp: gen_fcmp_op (expr); break;
  case LLVMSelect: {
    MIR_insn_code_t insn_code;
    MIR_label_t false_label = MIR_new_label (context), fin_label = MIR_new_label (context);

    op0 = LLVMGetOperand (expr, 0);
    op1 = LLVMGetOperand (expr, 1);
    op2 = LLVMGetOperand (expr, 2);
    type = LLVMTypeOf (op1);
    mir_type = get_mir_type (type);
    res_reg = get_expr_res_reg (expr, mir_var_type (mir_type));
    insn_code = mir_mov_code (mir_type);
    mir_op0 = get_mir_op (op0, MIR_T_I64);
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context, MIR_BF, MIR_new_label_op (context, false_label),
                                   mir_op0));
    mir_op1 = get_mir_op (op1, mir_type);
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context, insn_code, MIR_new_reg_op (context, res_reg), mir_op1));
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context, MIR_JMP, MIR_new_label_op (context, fin_label)));
    MIR_append_insn (context, curr_mir_func, false_label);
    mir_op2 = get_mir_op (op2, mir_type);
    MIR_append_insn (context, curr_mir_func,
                     MIR_new_insn (context, insn_code, MIR_new_reg_op (context, res_reg), mir_op2));
    MIR_append_insn (context, curr_mir_func, fin_label);
    break;
  }
    /* ???implement */
  case LLVMExtractValue:
  case LLVMInsertValue:
    error ("aggregate values and extract/insert value ops are not supported");
    break;

  default: error ("Unknow LLVM expr"); break;
  }
}

MIR_module_t llvm2mir (MIR_context_t c, LLVMModuleRef module) {
  int ptr_size;
  const char *id;
  size_t len;
  MIR_item_t item;
  MIR_var_t var;
  LLVMValueRef op0, op1, op2, param;
  MIR_type_t mir_type;
  MIR_op_t mir_op0, mir_op1;
  MIR_insn_code_t mir_insn_code;
  MIR_insn_t mir_insn, last_mir_insn;
  DLIST (MIR_insn_t) insns;
  LLVMTypeRef type;
  LLVMTypeKind type_id;
  LLVMOpcode opcode;
  LLVMLinkage linkage;
  MIR_reg_t res_reg, ptr_reg;
  unsigned proto_num = 0;
  unsigned long el_size;
  char buf[30];

  context = c;
  TD = LLVMGetModuleDataLayout (module);
  init_bb_gen_info ();
  HTAB_CREATE (expr_res_t, expr_res_tab, 512, expr_res_hash, expr_res_eq, NULL);
  HTAB_CREATE (item_t, item_tab, 64, item_hash, item_eq, NULL);
  id = LLVMGetModuleIdentifier (module, &len);
  curr_mir_module = MIR_new_module (context, id);
  ptr_size = LLVMPointerSize (TD);
  assert (ptr_size == 4 || ptr_size == 8);
  VARR_CREATE (char, string, 0);
  VARR_CREATE (MIR_var_t, mir_vars, 0);
  VARR_CREATE (MIR_op_t, mir_ops, 0);
  VARR_CREATE (LLVMTypeRef, types, 0);
  /* Loop through all globals in the module: */
  for (LLVMValueRef global = LLVMGetFirstGlobal (module); global;
       global = LLVMGetNextGlobal (global)) {
    linkage = LLVMGetLinkage (global);
    if (LLVMGetInitializer (global) == NULL && linkage == LLVMExternalLinkage) { /* ??? */
      item = MIR_new_import (context, LLVMGetValueName (global));
      add_item (item);
    } else if (linkage == LLVMPrivateLinkage || linkage == LLVMInternalLinkage
               || linkage == LLVMExternalLinkage || linkage == LLVMCommonLinkage) {
      type = LLVMTypeOf (global);
      type = LLVMGetElementType (type);
      item = gen_data_bss (type, LLVMGetValueName (global), LLVMGetInitializer (global));
      add_item (item);
      if (linkage == LLVMExternalLinkage || linkage == LLVMCommonLinkage) {
        item = MIR_new_export (context, LLVMGetValueName (global));
        add_item (item);
      }
    } else {
      assert (FALSE);
    }
  }
  /* Output forwards and prototypes */
  for (LLVMValueRef func = LLVMGetFirstFunction (module); func; func = LLVMGetNextFunction (func)) {
    id = LLVMGetValueName (func);
    if (intrinsic_p (id)) {
      if (ignored_intrinsic_p (id)) continue;
    }
    if (!LLVMIsDeclaration (func)) {
      add_item (MIR_new_forward (context, id));
    } else {
      linkage = LLVMGetLinkage (func);
      assert (linkage == LLVMExternalLinkage || linkage == LLVMExternalWeakLinkage); /* ??? */
      if (strcmp (id, "llvm.va_start") != 0 && strcmp (id, "llvm.va_end") != 0)
        add_item (MIR_new_import (context, id));
    }
  }
  /* Loop through all the functions in the module: */
  for (LLVMValueRef func = LLVMGetFirstFunction (module); func; func = LLVMGetNextFunction (func)) {
    LLVMTypeRef ftype = LLVMTypeOf (func);
    LLVMTypeRef ret_type;
    int nres;

    if (LLVMIsDeclaration (func)) continue;
    assert (LLVMGetTypeKind (ftype) == LLVMPointerTypeKind);
    ftype = LLVMGetElementType (ftype);
    ret_type = LLVMGetReturnType (ftype);
    if ((nres = LLVMGetTypeKind (ret_type) != LLVMVoidTypeKind)) mir_type = get_mir_type (ret_type);
    if (LLVMGetFunctionCallConv (func) != LLVMCCallConv
        && LLVMGetFunctionCallConv (func) != LLVMFastCallConv)
      error ("unsupported call convention");
    /* ??? func attrs */
    VARR_TRUNC (MIR_var_t, mir_vars, 0);
    curr_mir_func_reg_num = 0;
    for (unsigned i = 0; i < LLVMCountParams (func); i++) { /* ??? param attrs */
      param = LLVMGetParam (func, i);
      sprintf (buf, "%%%u", curr_mir_func_reg_num++);
      var.type = get_mir_type (LLVMTypeOf (param));
      var.name = _MIR_uniq_string (context, buf);
      VARR_PUSH (MIR_var_t, mir_vars, var);
    }
    curr_mir_func = ((LLVMIsFunctionVarArg (ftype)
                        ? MIR_new_vararg_func_arr
                        : MIR_new_func_arr) (context, LLVMGetValueName (func), nres, &mir_type,
                                             VARR_LENGTH (MIR_var_t, mir_vars),
                                             VARR_ADDR (MIR_var_t, mir_vars)));
    HTAB_CLEAR (expr_res_t, expr_res_tab);
    mir_int_temp_reg = MIR_new_func_reg (context, curr_mir_func->u.func, MIR_T_I64, "$temp");
    for (unsigned i = 0; i < LLVMCountParams (func); i++) {
      var = VARR_GET (MIR_var_t, mir_vars, i);
      add_mir_reg_to_table (LLVMGetParam (func, i),
                            MIR_reg (context, var.name, curr_mir_func->u.func));
    }
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock (func); bb;
         bb = LLVMGetNextBasicBlock (bb)) {
      /* Synchronize MIR and LLVM IR names.  Remember LLVM basic block is also a value. */
      curr_mir_func_reg_num++;
      for (LLVMValueRef insn = LLVMGetFirstInstruction (bb); insn;
           insn = LLVMGetNextInstruction (insn)) {
        type = LLVMTypeOf (insn);
        type_id = LLVMGetTypeKind (type);
        if (type_id == LLVMVoidTypeKind) continue;
        mir_type = get_mir_type (type);
        get_expr_res_reg (insn, mir_type);
      }
    }
    mir_2nd_mem_addr_reg = 0;
    init_phi_generation ();
    /* Loop through all the basic blocks in the function: */
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock (func); bb;
         bb = LLVMGetNextBasicBlock (bb)) {
      MIR_label_t bb_label = get_mir_bb_label (bb);

      MIR_append_insn (context, curr_mir_func, bb_label);
      /* Loop through all the instructions in the basic block: */
      for (LLVMValueRef insn = LLVMGetFirstInstruction (bb); insn;
           insn = LLVMGetNextInstruction (insn)) {
        opcode = LLVMGetInstructionOpcode (insn);
        switch (opcode) {
        default:
          process_expr (opcode, insn);
          break;
          /* Terminator Instructions */
        case LLVMRet: { /* ??? int size */
          int void_p = LLVMGetNumOperands (insn) == 0;

          if (void_p) {
            mir_insn = MIR_new_ret_insn (context, 0);
          } else {
            op0 = LLVMGetOperand (insn, 0);
            type = LLVMTypeOf (op0);
            type_id = LLVMGetTypeKind (type);
            mir_op0 = get_mir_op (op0, mir_type_of_type_id (type_id));
            mir_insn = MIR_new_ret_insn (context, 1, mir_op0);
          }
          MIR_append_insn (context, curr_mir_func, mir_insn);
          break;
        }
        case LLVMBr: {
          LLVMBasicBlockRef dest_bb, jump_bb;

          if (!LLVMIsConditional (insn)) { /* unconditional branch */
            assert (LLVMGetNumSuccessors (insn) == 1);
            dest_bb = LLVMGetSuccessor (insn, 0);
            mir_insn = MIR_new_insn (context, MIR_JMP,
                                     MIR_new_label_op (context, get_mir_bb_label (dest_bb)));
            MIR_append_insn (context, curr_mir_func, mir_insn);
            add_bb_dest (bb, dest_bb, mir_insn);
          } else {
            op0 = LLVMGetOperand (insn, 0);
            assert (LLVMGetNumSuccessors (insn) == 2);
            dest_bb = LLVMGetSuccessor (insn, 0); /* true branch */
            jump_bb = LLVMGetSuccessor (insn, 1); /* false branch */
            mir_insn_code = MIR_BTS;
            if (dest_bb == LLVMGetNextBasicBlock (bb)) {
              dest_bb = LLVMGetSuccessor (insn, 1);
              jump_bb = LLVMGetSuccessor (insn, 0);
              mir_insn_code = MIR_BFS;
            }
            mir_op0 = get_mir_op (op0, MIR_T_I64);
            mir_insn
              = MIR_new_insn (context, mir_insn_code,
                              MIR_new_label_op (context, get_mir_bb_label (dest_bb)), mir_op0);
            MIR_append_insn (context, curr_mir_func, mir_insn);
            add_bb_dest (bb, dest_bb, mir_insn);
            if (jump_bb == LLVMGetNextBasicBlock (bb)) {
              add_bb_dest (bb, jump_bb, NULL);
            } else {
              mir_insn = MIR_new_insn (context, MIR_JMP,
                                       MIR_new_label_op (context, get_mir_bb_label (jump_bb)));
              MIR_append_insn (context, curr_mir_func, mir_insn);
              add_bb_dest (bb, jump_bb, mir_insn);
            }
          }
          break;
        }
        case LLVMSwitch: {
          LLVMBasicBlockRef dest_bb;
          int short_p;

          op0 = LLVMGetOperand (insn, 0);
          type = LLVMTypeOf (op0);
          type_id = LLVMGetTypeKind (type);
          assert (type_id == LLVMIntegerTypeKind);
          short_p = LLVMGetIntTypeWidth (type) <= 32;
          mir_op0 = get_mir_op (op0, MIR_T_I64);
          for (unsigned i = 2; i < LLVMGetNumOperands (insn); i += 2) {
            LLVMValueRef caseval = LLVMGetOperand (insn, i);

            dest_bb = (LLVMBasicBlockRef) LLVMGetOperand (insn, i + 1);
            type = LLVMTypeOf (caseval);
            type_id = LLVMGetTypeKind (type);
            assert (type_id == LLVMIntegerTypeKind);
            mir_insn = MIR_new_insn (context, short_p ? MIR_BEQS : MIR_BEQ,
                                     MIR_new_label_op (context, get_mir_bb_label (dest_bb)),
                                     mir_op0, get_mir_op (caseval, MIR_T_I64));
            MIR_append_insn (context, curr_mir_func, mir_insn);
            add_bb_dest (bb, dest_bb, mir_insn);
          }
          dest_bb = LLVMGetSwitchDefaultDest (insn);
          mir_insn = MIR_new_insn (context, MIR_JMP,
                                   MIR_new_label_op (context, get_mir_bb_label (dest_bb)));
          MIR_append_insn (context, curr_mir_func, mir_insn);
          add_bb_dest (bb, dest_bb, mir_insn);
          break;
        }
        case LLVMIndirectBr: {
          error ("indirect branches are not implemented yet");
          break;
        }
        case LLVMInvoke: error ("exceptions are not implemented"); break;
        case LLVMPHI:
          type = LLVMTypeOf (insn);
          mir_type = get_mir_type (type);
          mir_type = mir_var_type (mir_type);
          res_reg = get_expr_res_reg (insn, mir_type);
          mir_insn_code = mir_mov_code (mir_type);
          for (unsigned i = 0; i < LLVMCountIncoming (insn); i++) {
            LLVMValueRef op = LLVMGetIncomingValue (insn, i);
            LLVMBasicBlockRef from_bb = LLVMGetIncomingBlock (insn, i);

            if (LLVMGetValueKind (op) == LLVMUndefValueValueKind) continue;
            last_mir_insn = DLIST_TAIL (MIR_insn_t, curr_mir_func->u.func->insns);
            MIR_append_insn (context, curr_mir_func,
                             MIR_new_insn (context, mir_insn_code,
                                           MIR_new_reg_op (context, res_reg),
                                           get_mir_op (op, mir_type)));
            DLIST_INIT (MIR_insn_t, insns);
            while ((mir_insn = DLIST_TAIL (MIR_insn_t, curr_mir_func->u.func->insns))
                   != last_mir_insn) {
              DLIST_REMOVE (MIR_insn_t, curr_mir_func->u.func->insns, mir_insn);
              DLIST_PREPEND (MIR_insn_t, insns, mir_insn);
            }
            add_phi_op_eval (from_bb, bb, insns);
          }
          break;
        case LLVMCall: { /* prototype ??? */
          LLVMValueRef func = LLVMGetCalledValue (insn);
          const char *func_name = get_func_name (func);
          unsigned conv = LLVMGetInstructionCallConv (insn);
          LLVMTypeRef ret_type, ftype = LLVMTypeOf (func);

          if (func_name != NULL) {
            if (ignored_intrinsic_p (func_name)) break;
            if (strcmp (func_name, "llvm.va_start") == 0
                || strcmp (func_name, "llvm.va_end") == 0) {
              assert (LLVMGetNumArgOperands (insn) == 1);
              mir_insn_code = strcmp (func_name, "llvm.va_start") == 0 ? MIR_VA_START : MIR_VA_END;
              op0 = LLVMGetOperand (insn, 0);
              MIR_append_insn (context, curr_mir_func,
                               MIR_new_insn (context, mir_insn_code, get_mir_op (op0, MIR_T_I64)));
              break;
            }
          }
          assert (LLVMGetTypeKind (ftype) == LLVMPointerTypeKind);
          ftype = LLVMGetElementType (ftype);
          ret_type = LLVMGetReturnType (ftype);
          if (conv != LLVMCCallConv && conv != LLVMFastCallConv)
            error ("unsupported call convention");
#if 0
	  if (LLVMIsFunctionVarArg (ftype))
	    error ("var arg func call is not supported yet");
#endif
          VARR_TRUNC (MIR_op_t, mir_ops, 0);
          VARR_PUSH (MIR_op_t, mir_ops, MIR_new_ref_op (context, get_proto (ftype, &proto_num)));
          VARR_PUSH (MIR_op_t, mir_ops, get_mir_op (func, MIR_T_I64));
          if (LLVMGetTypeKind (ret_type) != LLVMVoidTypeKind)
            VARR_PUSH (MIR_op_t, mir_ops,
                       MIR_new_reg_op (context, get_expr_res_reg (insn, get_mir_type (ret_type))));
          for (unsigned i = 0; i < LLVMGetNumArgOperands (insn); i++) {
            op0 = LLVMGetOperand (insn, i);
            type = LLVMTypeOf (op0);
            type_id = LLVMGetTypeKind (type);
            mir_type = mir_type_of_type_id (type_id);
            VARR_PUSH (MIR_op_t, mir_ops, get_mir_op (op0, mir_type));
          }
          MIR_append_insn (context, curr_mir_func,
                           MIR_new_insn_arr (context, MIR_CALL, VARR_LENGTH (MIR_op_t, mir_ops),
                                             VARR_ADDR (MIR_op_t, mir_ops)));
          break;
        }
        case LLVMUnreachable:
          /* ??? should we use this info in MIR */
          break;

          /* Memory Operators */
        case LLVMAlloca:
          op0 = LLVMGetOperand (insn, 0);
          type = LLVMTypeOf (op0);
          type_id = LLVMGetTypeKind (type);
          assert (type_id == LLVMIntegerTypeKind);
          type = LLVMTypeOf (insn);
          type = LLVMGetElementType (type);
          el_size = LLVMABISizeOfType (TD, type);
          res_reg = get_expr_res_reg (insn, MIR_T_I64);
          mir_op0 = get_mir_op (op0, MIR_T_I64);
          if (el_size != 1) {
            if (mir_op0.mode == MIR_OP_INT) {
              mir_op0 = MIR_new_int_op (context, el_size * mir_op0.u.i);
            } else {
              MIR_append_insn (context, curr_mir_func,
                               MIR_new_insn (context, MIR_MUL, MIR_new_reg_op (context, res_reg),
                                             mir_op0, MIR_new_int_op (context, el_size)));
              mir_op0 = MIR_new_reg_op (context, res_reg);
            }
          }
          MIR_append_insn (context, curr_mir_func,
                           MIR_new_insn (context, MIR_ALLOCA, MIR_new_reg_op (context, res_reg),
                                         mir_op0));
          break;
        case LLVMLoad: { /* ??? attrs, signdness */
          assert (LLVMGetNumOperands (insn) == 1);
          op0 = LLVMGetOperand (insn, 0);
          type = LLVMTypeOf (op0);
          type_id = LLVMGetTypeKind (type);
          assert (type_id == LLVMPointerTypeKind);
          type = LLVMGetElementType (type);
          mir_type = get_mir_type (type);
          res_reg = get_expr_res_reg (insn, mir_var_type (mir_type));
          mir_op0 = get_mir_op (op0, mir_type);
          ptr_reg = force_ptr_to_reg (mir_op0);
          MIR_append_insn (context, curr_mir_func,
                           MIR_new_insn (context, mir_mov_code (mir_type),
                                         MIR_new_reg_op (context, res_reg),
                                         MIR_new_mem_op (context, mir_type, 0, ptr_reg, 0, 1)));
          break;
        }
        case LLVMStore: { /* ??? attrs */
          assert (LLVMGetNumOperands (insn) == 2);
          op0 = LLVMGetOperand (insn, 0);
          op1 = LLVMGetOperand (insn, 1);
          type = LLVMTypeOf (op0);
          type_id = LLVMGetTypeKind (type);
          assert (LLVMGetTypeKind (LLVMTypeOf (op1)) == LLVMPointerTypeKind);
          mir_type = get_mir_type (type);
          mir_op0 = get_mir_op (op0, mir_type);
          mir_op1 = get_mir_op (op1, mir_type);
          ptr_reg = force_ptr_to_reg (mir_op1);
          MIR_append_insn (context, curr_mir_func,
                           MIR_new_insn (context, mir_mov_code (mir_type),
                                         MIR_new_mem_op (context, mir_type, 0, ptr_reg, 0, 1),
                                         mir_op0));
          break;
        }
        case LLVMUserOp1:
        case LLVMUserOp2: error ("user op should be not here"); break;
        case LLVMVAArg: error ("varg is not implemented yet"); break;
        case LLVMExtractElement:
        case LLVMInsertElement:
        case LLVMShuffleVector:
          error ("vectors are not implemented: don't use autovectorization");
          break;
          /* Atomic operators */
        case LLVMFence:
        case LLVMAtomicCmpXchg:
        case LLVMAtomicRMW:
          error ("atomic operations are not implemented");
          break;

          /* Exception Handling Operators */
        case LLVMResume:
        case LLVMLandingPad:
        case LLVMCleanupRet:
        case LLVMCatchRet:
        case LLVMCatchPad:
        case LLVMCleanupPad:
        case LLVMCatchSwitch: error ("exceptions are not implemented"); break;
        }
      }
      last_mir_insn = DLIST_TAIL (MIR_insn_t, curr_mir_func->u.func->insns);
      if (last_mir_insn != bb_label) update_last_bb_insn (bb, last_mir_insn);
    }
    for (size_t i = 0; i < VARR_LENGTH (bb_gen_info_t, bb_gen_infos);
         i++) /* Finish processing phi nodes: */
      generate_edge_phi_op_eval (VARR_ADDR (bb_gen_info_t, bb_gen_infos)[i]);
    VARR_TRUNC (bb_gen_info_t, bb_gen_infos, 0);
    finish_phi_generation ();
    MIR_finish_func (context);
  }

  MIR_finish_module (context);
  finish_bb_gen_info ();
  HTAB_DESTROY (expr_res_t, expr_res_tab);
  HTAB_DESTROY (item_t, item_tab);
  VARR_DESTROY (MIR_var_t, mir_vars);
  VARR_DESTROY (MIR_op_t, mir_ops);
  VARR_DESTROY (LLVMTypeRef, types);
  VARR_DESTROY (char, string);
  return curr_mir_module;
}
