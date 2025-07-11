/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
   riscv64 call ABI target specific code.
*/

typedef struct target_arg_info {
  int n_iregs, n_fregs;
} target_arg_info_t;

static void target_init_arg_vars (c2m_ctx_t c2m_ctx MIR_UNUSED, target_arg_info_t *arg_info) {
  arg_info->n_iregs = arg_info->n_fregs = 0;
}

static int target_return_by_addr_p (c2m_ctx_t c2m_ctx, struct type *ret_type) {
  return ((ret_type->mode == TM_STRUCT || ret_type->mode == TM_UNION)
          && type_size (c2m_ctx, ret_type) > 2 * 8);
}

static int reg_aggregate_size (c2m_ctx_t c2m_ctx, struct type *type) {
  size_t size;

  if (type->mode != TM_STRUCT && type->mode != TM_UNION) return -1;
  return (size = type_size (c2m_ctx, type)) <= 2 * 8 ? (int) size : -1;
}

#define MAX_MEMBERS 2

struct type_offset {
  uint64_t offset;
  MIR_type_t type; /* gcc uses unsigned to pass integer members of
                      mixed int/float type.  so it is unsigned for
                      any 32-bit or less int type */
};

static int small_struct_p (c2m_ctx_t c2m_ctx, struct type *type, int struct_only_p,
                           uint64_t start_offset, int *members_n,
                           struct type_offset members[MAX_MEMBERS]) {
  MIR_type_t mir_type;
  struct type_offset sub_members[MAX_MEMBERS];
  int sub_n;

  if (!struct_only_p && scalar_type_p (type)) {
    mir_type = get_mir_type (c2m_ctx, type);
    members[0].type = mir_type == MIR_T_I8    ? MIR_T_U8
                      : mir_type == MIR_T_I16 ? MIR_T_U16
                      : mir_type == MIR_T_I32 ? MIR_T_U32
                                              : mir_type;
    members[0].offset = start_offset;
    *members_n = 1;
  } else if (!struct_only_p && type->mode == TM_ARR) {
    struct arr_type *at = type->u.arr_type;
    struct expr *cexpr;
    uint64_t nel;

    if (at->size->code == N_IGNORE || !(cexpr = at->size->attr)->const_p) return FALSE;
    nel = cexpr->c.i_val;
    if (!small_struct_p (c2m_ctx, at->el_type, FALSE, 0, &sub_n, sub_members)) return FALSE;
    if (sub_n * nel > 2) return FALSE;
    for (size_t i = 0; i < sub_n * nel; i++) {
      members[i].type = sub_members[i].type;
      members[i].offset = start_offset + i * type_size (c2m_ctx, at->el_type);
    }
    *members_n = sub_n * nel;
  } else if (type->mode != TM_STRUCT) {
    return FALSE;
  } else {
    *members_n = 0;
    for (node_t el = NL_HEAD (NL_EL (type->u.tag_type->u.ops, 1)->u.ops); el != NULL;
         el = NL_NEXT (el))
      if (el->code == N_MEMBER) {
        decl_t decl = el->attr;
        mir_size_t member_offset = decl->offset;

        if (decl->width == 0) continue;
        if (decl->containing_unnamed_anon_struct_union_member != NULL) {
          member_offset = 0;
        }
        if (!small_struct_p (c2m_ctx, decl->decl_spec.type, FALSE, member_offset + start_offset,
                             &sub_n, sub_members))
          return FALSE;
        if (sub_n + *members_n > 2) return FALSE;
        for (int i = 0; i < sub_n; i++) members[*members_n + i] = sub_members[i];
        if (decl->width > 0) {
          assert (sub_n == 1);
          members[*members_n].type = (decl->width <= 8    ? MIR_T_U8
                                      : decl->width <= 16 ? MIR_T_U16
                                      : decl->width <= 32 ? MIR_T_U32
                                                          : MIR_T_U64);
        }
        *members_n += sub_n;
      }
  }
  return TRUE;
}

static int small_fp_struct_p (c2m_ctx_t c2m_ctx, struct type *type, int *members_n,
                              struct type_offset members[MAX_MEMBERS]) {
  if (!small_struct_p (c2m_ctx, type, TRUE, 0, members_n, members)) return FALSE;
  for (int i = 0; i < *members_n; i++)
    if (members[i].type == MIR_T_F || members[i].type == MIR_T_D) return TRUE;
  return FALSE;
}

static void target_add_res_proto (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                  target_arg_info_t *arg_info, VARR (MIR_type_t) * res_types,
                                  VARR (MIR_var_t) * arg_vars) {
  MIR_var_t var;
  MIR_type_t type;
  int size, n;
  struct type_offset members[MAX_MEMBERS];

  if (void_type_p (ret_type)) return;
  if ((size = reg_aggregate_size (c2m_ctx, ret_type)) < 0) {
    if (ret_type->mode != TM_STRUCT && ret_type->mode != TM_UNION) {
      type = get_mir_type (c2m_ctx, ret_type);
      VARR_PUSH (MIR_type_t, res_types, type);
    } else { /* return by reference */
      var.name = RET_ADDR_NAME;
      var.type = MIR_T_RBLK;
      var.size = type_size (c2m_ctx, ret_type);
      VARR_PUSH (MIR_var_t, arg_vars, var);
      arg_info->n_iregs++;
    }
    return;
  }
  if (size == 0) return;
  if (small_fp_struct_p (c2m_ctx, ret_type, &n, members)) {
    VARR_PUSH (MIR_type_t, res_types, members[0].type);
    if (n > 1) VARR_PUSH (MIR_type_t, res_types, members[1].type);
  } else {
    VARR_PUSH (MIR_type_t, res_types, MIR_T_I64);
    if (size > 8) VARR_PUSH (MIR_type_t, res_types, MIR_T_I64);
  }
}

static int target_add_call_res_op (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                   target_arg_info_t *arg_info, size_t call_arg_area_offset) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  struct type_offset members[MAX_MEMBERS];
  int size, n;
  op_t temp;
  MIR_type_t type;

  if (void_type_p (ret_type)) return -1;
  if ((size = reg_aggregate_size (c2m_ctx, ret_type)) < 0) {
    if (ret_type->mode == TM_STRUCT || ret_type->mode == TM_UNION) { /* return by reference */
      arg_info->n_iregs++;
      temp = get_new_temp (c2m_ctx, MIR_T_I64);
      emit3 (c2m_ctx, MIR_ADD, temp.mir_op,
             MIR_new_reg_op (ctx, MIR_reg (ctx, FP_NAME, curr_func->u.func)),
             MIR_new_int_op (ctx, call_arg_area_offset));
      temp.mir_op
        = MIR_new_mem_op (ctx, MIR_T_RBLK, type_size (c2m_ctx, ret_type), temp.mir_op.u.reg, 0, 1);
      VARR_PUSH (MIR_op_t, call_ops, temp.mir_op);
      return 0;
    } else {
      type = get_mir_type (c2m_ctx, ret_type);
      type = promote_mir_int_type (type);
      temp = get_new_temp (c2m_ctx, type);
      VARR_PUSH (MIR_op_t, call_ops, temp.mir_op);
      return 1;
    }
  }
  if (size == 0) return -1;
  if (small_fp_struct_p (c2m_ctx, ret_type, &n, members)) {
    VARR_PUSH (MIR_op_t, call_ops,
               MIR_new_reg_op (ctx, get_new_temp (c2m_ctx, promote_mir_int_type (members[0].type))
                                      .mir_op.u.reg));
    if (n > 1)
      VARR_PUSH (MIR_op_t, call_ops,
                 MIR_new_reg_op (ctx, get_new_temp (c2m_ctx, promote_mir_int_type (members[1].type))
                                        .mir_op.u.reg));
    return n;
  } else {
    VARR_PUSH (MIR_op_t, call_ops,
               MIR_new_reg_op (ctx, get_new_temp (c2m_ctx, MIR_T_I64).mir_op.u.reg));
    if (size > 8)
      VARR_PUSH (MIR_op_t, call_ops,
                 MIR_new_reg_op (ctx, get_new_temp (c2m_ctx, MIR_T_I64).mir_op.u.reg));
    return size <= 8 ? 1 : 2;
  }
}

static op_t target_gen_post_call_res_code (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res,
                                           MIR_insn_t call MIR_UNUSED, size_t call_ops_start) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  struct type_offset members[MAX_MEMBERS];
  MIR_insn_t insn;
  int size, n;

  if (void_type_p (ret_type)) return res;
  if ((size = reg_aggregate_size (c2m_ctx, ret_type)) < 0) return res;
  if (size == 0) return res;
  assert (res.mir_op.mode == MIR_OP_MEM);
  if (small_fp_struct_p (c2m_ctx, ret_type, &n, members)) {
    assert (n == 1 || n == 2);
    insn = MIR_new_insn (ctx, tp_mov (members[0].type),
                         MIR_new_mem_op (ctx, members[0].type,
                                         res.mir_op.u.mem.disp + (MIR_disp_t) members[0].offset,
                                         res.mir_op.u.mem.base, res.mir_op.u.mem.index,
                                         res.mir_op.u.mem.scale),
                         VARR_GET (MIR_op_t, call_ops, call_ops_start + 2));
    MIR_append_insn (ctx, curr_func, insn);
    if (n > 1) {
      insn = MIR_new_insn (ctx, tp_mov (members[1].type),
                           MIR_new_mem_op (ctx, members[1].type,
                                           res.mir_op.u.mem.disp + (MIR_disp_t) members[1].offset,
                                           res.mir_op.u.mem.base, res.mir_op.u.mem.index,
                                           res.mir_op.u.mem.scale),
                           VARR_GET (MIR_op_t, call_ops, call_ops_start + 3));
      MIR_append_insn (ctx, curr_func, insn);
    }
  } else {
    gen_multiple_load_store (c2m_ctx, ret_type, &VARR_ADDR (MIR_op_t, call_ops)[call_ops_start + 2],
                             res.mir_op, FALSE);
  }
  return res;
}

static void target_add_ret_ops (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  int i, n, size;
  struct type_offset members[MAX_MEMBERS];
  MIR_insn_t insn;
  op_t temp, var;
  MIR_reg_t ret_addr_reg;

  if (void_type_p (ret_type)) return;
  if ((size = reg_aggregate_size (c2m_ctx, ret_type)) < 0) {
    if (ret_type->mode != TM_STRUCT && ret_type->mode != TM_UNION) {
      VARR_PUSH (MIR_op_t, ret_ops, res.mir_op);
    } else {
      ret_addr_reg = MIR_reg (ctx, RET_ADDR_NAME, curr_func->u.func);
      var = new_op (NULL, MIR_new_mem_op (ctx, MIR_T_I8, 0, ret_addr_reg, 0, 1));
      block_move (c2m_ctx, var, res, type_size (c2m_ctx, ret_type));
    }
    return;
  }
  if (size == 0) return;
  assert (res.mir_op.mode == MIR_OP_MEM && VARR_LENGTH (MIR_op_t, ret_ops) == 0 && size <= 2 * 8);
  if (small_fp_struct_p (c2m_ctx, ret_type, &n, members)) {
    assert (n == 1 || n == 2);
    temp = get_new_temp (c2m_ctx, promote_mir_int_type (members[0].type));
    insn = MIR_new_insn (ctx, tp_mov (members[0].type), temp.mir_op,
                         MIR_new_mem_op (ctx, members[0].type,
                                         res.mir_op.u.mem.disp + (MIR_disp_t) members[0].offset,
                                         res.mir_op.u.mem.base, res.mir_op.u.mem.index,
                                         res.mir_op.u.mem.scale));
    MIR_append_insn (ctx, curr_func, insn);
    VARR_PUSH (MIR_op_t, ret_ops, temp.mir_op);
    if (n > 1) {
      temp = get_new_temp (c2m_ctx, promote_mir_int_type (members[1].type));
      insn = MIR_new_insn (ctx, tp_mov (members[1].type), temp.mir_op,
                           MIR_new_mem_op (ctx, members[1].type,
                                           res.mir_op.u.mem.disp + (MIR_disp_t) members[1].offset,
                                           res.mir_op.u.mem.base, res.mir_op.u.mem.index,
                                           res.mir_op.u.mem.scale));
      MIR_append_insn (ctx, curr_func, insn);
      VARR_PUSH (MIR_op_t, ret_ops, temp.mir_op);
    }
  } else {
    for (i = 0; size > 0; size -= 8, i++)
      VARR_PUSH (MIR_op_t, ret_ops, get_new_temp (c2m_ctx, MIR_T_I64).mir_op);
    gen_multiple_load_store (c2m_ctx, ret_type, VARR_ADDR (MIR_op_t, ret_ops), res.mir_op, TRUE);
  }
}

static MIR_type_t target_get_blk_type (c2m_ctx_t c2m_ctx, struct type *arg_type) {
  assert (arg_type->mode == TM_STRUCT || arg_type->mode == TM_UNION);
  return simple_target_get_blk_type (c2m_ctx, arg_type);
}

static void target_add_arg_proto (c2m_ctx_t c2m_ctx, const char *name, struct type *arg_type,
                                  target_arg_info_t *arg_info, VARR (MIR_var_t) * arg_vars) {
  MIR_var_t var;
  MIR_type_t type;
  int size, n;
  struct type_offset members[MAX_MEMBERS];

  /* pass aggregates on the stack and pass by value for others: */
  var.name = name;
  if (arg_type->mode != TM_STRUCT && arg_type->mode != TM_UNION) {
    type = get_mir_type (c2m_ctx, arg_type);
    var.type = type;
    if (type == MIR_T_F || type == MIR_T_D)
      arg_info->n_fregs++;
    else if (type != MIR_T_LD)
      arg_info->n_iregs++;
    else
      arg_info->n_iregs += 2;
    VARR_PUSH (MIR_var_t, arg_vars, var);
  } else if ((size = reg_aggregate_size (c2m_ctx, arg_type)) < 0) { /* big struct -- pass address */
    var.type = target_get_blk_type (c2m_ctx, arg_type);
    var.size = type_size (c2m_ctx, arg_type);
    arg_info->n_iregs++;
    VARR_PUSH (MIR_var_t, arg_vars, var);
  } else {
    if (small_fp_struct_p (c2m_ctx, arg_type, &n, members)) {
      int n_fp = 0, n_int = 0;
      if (members[0].type == MIR_T_F || members[0].type == MIR_T_D)
        n_fp++;
      else
        n_int++;
      if (n == 2) {
        if (members[1].type == MIR_T_F || members[1].type == MIR_T_D)
          n_fp++;
        else
          n_int++;
      }
      if (arg_info->n_iregs + n_int <= 8 && arg_info->n_fregs + n_fp <= 8) {
        arg_info->n_iregs += n_int;
        arg_info->n_fregs += n_fp;
        var.type = members[0].type;
        var.name = gen_get_indexed_name (c2m_ctx, name, 0);
        VARR_PUSH (MIR_var_t, arg_vars, var);
        if (n == 2) {
          var.type = members[1].type;
          var.name = gen_get_indexed_name (c2m_ctx, name, 1);
          VARR_PUSH (MIR_var_t, arg_vars, var);
        }
        return;
      }
    }
    var.type = target_get_blk_type (c2m_ctx, arg_type);
    var.size = type_size (c2m_ctx, arg_type);
    VARR_PUSH (MIR_var_t, arg_vars, var);
    arg_info->n_iregs += size <= 8 ? 1 : 2;
  }
}

static void target_add_call_arg_op (c2m_ctx_t c2m_ctx, struct type *arg_type,
                                    target_arg_info_t *arg_info, op_t arg) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_type_t type;
  int size, n;
  struct type_offset members[MAX_MEMBERS];

  /* pass aggregates on the stack and pass by value for others: */
  if (arg_type->mode != TM_STRUCT && arg_type->mode != TM_UNION) {
    type = get_mir_type (c2m_ctx, arg_type);
    if (type == MIR_T_F || type == MIR_T_D)
      arg_info->n_fregs++;
    else if (type != MIR_T_LD)
      arg_info->n_iregs++;
    else
      arg_info->n_iregs += 2;
    VARR_PUSH (MIR_op_t, call_ops, arg.mir_op);
    return;
  }
  assert (arg.mir_op.mode == MIR_OP_MEM);
  arg = mem_to_address (c2m_ctx, arg, TRUE);
  if ((size = reg_aggregate_size (c2m_ctx, arg_type)) < 0) { /* big struct -- pass address */
    arg_info->n_iregs++;
    type = target_get_blk_type (c2m_ctx, arg_type);
    VARR_PUSH (MIR_op_t, call_ops,
               MIR_new_mem_op (ctx, type, type_size (c2m_ctx, arg_type), arg.mir_op.u.reg, 0, 1));
    return;
  }
  if (small_fp_struct_p (c2m_ctx, arg_type, &n, members)) {
    int n_fp = 0, n_int = 0;
    if (members[0].type == MIR_T_F || members[0].type == MIR_T_D)
      n_fp++;
    else
      n_int++;
    if (n == 2) {
      if (members[1].type == MIR_T_F || members[1].type == MIR_T_D)
        n_fp++;
      else
        n_int++;
    }
    if (arg_info->n_iregs + n_int <= 8 && arg_info->n_fregs + n_fp <= 8) {
      arg_info->n_iregs += n_int;
      arg_info->n_fregs += n_fp;
      VARR_PUSH (MIR_op_t, call_ops,
                 MIR_new_mem_op (ctx, members[0].type, members[0].offset, arg.mir_op.u.reg, 0, 1));
      if (n == 2) {
        VARR_PUSH (MIR_op_t, call_ops,
                   MIR_new_mem_op (ctx, members[1].type, members[1].offset, arg.mir_op.u.reg, 0,
                                   1));
      }
      return;
    }
  }
  arg_info->n_iregs += size <= 8 ? 1 : 2;
  type = target_get_blk_type (c2m_ctx, arg_type);
  VARR_PUSH (MIR_op_t, call_ops,
             MIR_new_mem_op (ctx, type, type_size (c2m_ctx, arg_type), arg.mir_op.u.reg, 0, 1));
}

static int target_gen_gather_arg (c2m_ctx_t c2m_ctx, const char *name, struct type *arg_type,
                                  decl_t param_decl, target_arg_info_t *arg_info) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_type_t type;
  int size, n;
  struct type_offset members[MAX_MEMBERS];
  reg_var_t reg_var;

  /* pass aggregates on the stack and pass by value for others: */
  if (arg_type->mode != TM_STRUCT && arg_type->mode != TM_UNION) {
    type = get_mir_type (c2m_ctx, arg_type);
    if (type == MIR_T_F || type == MIR_T_D)
      arg_info->n_fregs++;
    else if (type != MIR_T_LD)
      arg_info->n_iregs++;
    else
      arg_info->n_iregs += 2;
    return FALSE;
  }
  if ((size = reg_aggregate_size (c2m_ctx, arg_type))
      < 0) { /* big struct -- pass address ???? copy */
    arg_info->n_iregs++;
    return FALSE;
  }
  if (small_fp_struct_p (c2m_ctx, arg_type, &n, members)) {
    int n_fp = 0, n_int = 0;
    if (members[0].type == MIR_T_F || members[0].type == MIR_T_D)
      n_fp++;
    else
      n_int++;
    if (n == 2) {
      if (members[1].type == MIR_T_F || members[1].type == MIR_T_D)
        n_fp++;
      else
        n_int++;
    }
    if (arg_info->n_iregs + n_int <= 8 && arg_info->n_fregs + n_fp <= 8) {
      arg_info->n_iregs += n_int;
      arg_info->n_fregs += n_fp;
      assert (!param_decl->reg_p);
      type = members[0].type;
      reg_var = get_reg_var (c2m_ctx, promote_mir_int_type (type),
                             gen_get_indexed_name (c2m_ctx, name, 0), NULL);
      MIR_append_insn (ctx, curr_func,
                       MIR_new_insn (ctx, tp_mov (type),
                                     MIR_new_mem_op (ctx, type,
                                                     param_decl->offset + members[0].offset,
                                                     MIR_reg (ctx, FP_NAME, curr_func->u.func), 0,
                                                     1),
                                     MIR_new_reg_op (ctx, reg_var.reg)));
      if (n == 2) {
        type = members[1].type;
        reg_var = get_reg_var (c2m_ctx, promote_mir_int_type (type),
                               gen_get_indexed_name (c2m_ctx, name, 1), NULL);
        MIR_append_insn (ctx, curr_func,
                         MIR_new_insn (ctx, tp_mov (type),
                                       MIR_new_mem_op (ctx, type,
                                                       param_decl->offset + members[1].offset,
                                                       MIR_reg (ctx, FP_NAME, curr_func->u.func), 0,
                                                       1),
                                       MIR_new_reg_op (ctx, reg_var.reg)));
      }
      return TRUE;
    }
  }
  arg_info->n_iregs += size <= 8 ? 1 : 2;
  return FALSE;
}
