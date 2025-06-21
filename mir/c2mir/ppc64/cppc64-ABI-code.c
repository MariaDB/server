/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
   ppc64 call ABI target specific code.
*/

typedef int target_arg_info_t;

static void target_init_arg_vars (c2m_ctx_t c2m_ctx MIR_UNUSED,
                                  target_arg_info_t *arg_info MIR_UNUSED) {}

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
static MIR_type_t fp_homogeneous_type (c2m_ctx_t c2m_ctx, struct type *param_type, int *num) {
  return MIR_T_UNDEF;
}
#else
static MIR_type_t fp_homogeneous_type_1 (c2m_ctx_t c2m_ctx, MIR_type_t curr_type, struct type *type,
                                         int *num) {
  int n;
  MIR_type_t t;

  if (type->mode == TM_STRUCT || type->mode == TM_UNION || type->mode == TM_ARR) {
    switch (type->mode) {
    case TM_ARR: { /* Arrays are handled as small records.  */
      struct arr_type *arr_type = type->u.arr_type;
      struct expr *cexpr = arr_type->size->attr;

      if ((t = fp_homogeneous_type_1 (c2m_ctx, curr_type, type->u.arr_type->el_type, &n))
          == MIR_T_UNDEF)
        return MIR_T_UNDEF;
      *num = arr_type->size->code == N_IGNORE || !cexpr->const_p ? 1 : cexpr->c.i_val;
      return t;
    }
    case TM_STRUCT:
    case TM_UNION:
      t = curr_type;
      *num = 0;
      for (node_t el = NL_HEAD (NL_EL (type->u.tag_type->u.ops, 1)->u.ops); el != NULL;
           el = NL_NEXT (el))
        if (el->code == N_MEMBER) {
          decl_t decl = el->attr;

          if ((t = fp_homogeneous_type_1 (c2m_ctx, t, decl->decl_spec.type, &n)) == MIR_T_UNDEF)
            return MIR_T_UNDEF;
          if (type->mode == TM_STRUCT)
            *num += n;
          else if (*num < n)
            *num = n;
        }
      return t;
    default: assert (FALSE);
    }
  }

  assert (scalar_type_p (type));
  if ((t = get_mir_type (c2m_ctx, type)) != MIR_T_F && t != MIR_T_D) return MIR_T_UNDEF;
  if (curr_type != t && curr_type != MIR_T_UNDEF) return MIR_T_UNDEF;
  *num = 1;
  return t;
}

static MIR_type_t fp_homogeneous_type (c2m_ctx_t c2m_ctx, struct type *param_type, int *num) {
  if (param_type->mode != TM_STRUCT && param_type->mode != TM_UNION) return MIR_T_UNDEF;
  return fp_homogeneous_type_1 (c2m_ctx, MIR_T_UNDEF, param_type, num);
}
#endif

static int reg_aggregate_p (c2m_ctx_t c2m_ctx, struct type *ret_type) {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return FALSE;
#else
  return type_size (c2m_ctx, ret_type) <= 2 * 8;
#endif
}

static int target_return_by_addr_p (c2m_ctx_t c2m_ctx, struct type *ret_type) {
  MIR_type_t type;
  int n;

  if (ret_type->mode != TM_STRUCT && ret_type->mode != TM_UNION) return FALSE;
  if (((type = fp_homogeneous_type (c2m_ctx, ret_type, &n)) == MIR_T_F || type == MIR_T_D)
      && n <= 8)
    return FALSE;
  return !reg_aggregate_p (c2m_ctx, ret_type);
}

static void target_add_res_proto (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                  target_arg_info_t *arg_info MIR_UNUSED,
                                  VARR (MIR_type_t) * res_types, VARR (MIR_var_t) * arg_vars) {
  MIR_var_t var;
  MIR_type_t type;
  int i, n, size;

  if (void_type_p (ret_type)) return;
  if (((type = fp_homogeneous_type (c2m_ctx, ret_type, &n)) == MIR_T_F || type == MIR_T_D)
      && n <= 8) {
    for (i = 0; i < n; i++) VARR_PUSH (MIR_type_t, res_types, type);
  } else if (ret_type->mode != TM_STRUCT && ret_type->mode != TM_UNION) {
    VARR_PUSH (MIR_type_t, res_types, get_mir_type (c2m_ctx, ret_type));
  } else if (reg_aggregate_p (c2m_ctx, ret_type)) {
    size = type_size (c2m_ctx, ret_type);
    for (; size > 0; size -= 8) VARR_PUSH (MIR_type_t, res_types, MIR_T_I64);
  } else {
    var.name = RET_ADDR_NAME;
    var.type = MIR_T_RBLK;
    var.size = type_size (c2m_ctx, ret_type);
    VARR_PUSH (MIR_var_t, arg_vars, var);
  }
}

static int target_add_call_res_op (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                   target_arg_info_t *arg_info MIR_UNUSED,
                                   size_t call_arg_area_offset) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_type_t type;
  op_t temp;
  int i, n, size;

  if (void_type_p (ret_type)) return -1;
  if (((type = fp_homogeneous_type (c2m_ctx, ret_type, &n)) == MIR_T_F || type == MIR_T_D)
      && n <= 8) {
    for (i = 0; i < n; i++) {
      temp = get_new_temp (c2m_ctx, type);
      VARR_PUSH (MIR_op_t, call_ops, temp.mir_op);
    }
    return n;
  } else if (ret_type->mode != TM_STRUCT && ret_type->mode != TM_UNION) {
    type = get_mir_type (c2m_ctx, ret_type);
    type = promote_mir_int_type (type);
    temp = get_new_temp (c2m_ctx, type);
    VARR_PUSH (MIR_op_t, call_ops, temp.mir_op);
    return 1;
  } else if (reg_aggregate_p (c2m_ctx, ret_type)) {
    size = type_size (c2m_ctx, ret_type);
    if (size == 0) return -1;
    for (int s = size; s > 0; s -= 8) {
      temp = get_new_temp (c2m_ctx, MIR_T_I64);
      VARR_PUSH (MIR_op_t, call_ops, temp.mir_op);
    }
    return (size + 7) / 8;
  } else {
    temp = get_new_temp (c2m_ctx, MIR_T_I64);
    emit3 (c2m_ctx, MIR_ADD, temp.mir_op,
           MIR_new_reg_op (ctx, MIR_reg (ctx, FP_NAME, curr_func->u.func)),
           MIR_new_int_op (ctx, call_arg_area_offset));
    temp.mir_op
      = MIR_new_mem_op (ctx, MIR_T_RBLK, type_size (c2m_ctx, ret_type), temp.mir_op.u.reg, 0, 1);
    VARR_PUSH (MIR_op_t, call_ops, temp.mir_op);
    return 0;
  }
}

static op_t target_gen_post_call_res_code (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res,
                                           MIR_insn_t call MIR_UNUSED, size_t call_ops_start) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_type_t type;
  MIR_insn_t insn;
  int i, n;

  if (void_type_p (ret_type)) return res;
  if (((type = fp_homogeneous_type (c2m_ctx, ret_type, &n)) == MIR_T_F || type == MIR_T_D)
      && n <= 8) {
    assert (res.mir_op.mode == MIR_OP_MEM);
    for (i = 0; i < n; i++) {
      insn = MIR_new_insn (ctx, tp_mov (type),
                           MIR_new_mem_op (ctx, type,
                                           res.mir_op.u.mem.disp + (type == MIR_T_F ? 4 : 8) * i,
                                           res.mir_op.u.mem.base, res.mir_op.u.mem.index,
                                           res.mir_op.u.mem.scale),
                           VARR_GET (MIR_op_t, call_ops, i + call_ops_start + 2));
      MIR_append_insn (ctx, curr_func, insn);
    }
  } else if ((ret_type->mode == TM_STRUCT || ret_type->mode == TM_UNION)
             && reg_aggregate_p (c2m_ctx, ret_type)) {
    assert (res.mir_op.mode == MIR_OP_MEM); /* addr */
    gen_multiple_load_store (c2m_ctx, ret_type, &VARR_ADDR (MIR_op_t, call_ops)[call_ops_start + 2],
                             res.mir_op, FALSE);
  }
  return res;
}

static void target_add_ret_ops (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_type_t type;
  MIR_insn_t insn;
  MIR_reg_t ret_addr_reg;
  op_t temp, var;
  int n, size;

  if (void_type_p (ret_type)) return;
  if (((type = fp_homogeneous_type (c2m_ctx, ret_type, &n)) == MIR_T_F || type == MIR_T_D)
      && n <= 8) {
    assert (res.mir_op.mode == MIR_OP_MEM);
    for (int i = 0; i < n; i++) {
      temp = get_new_temp (c2m_ctx, type);
      insn = MIR_new_insn (ctx, tp_mov (type), temp.mir_op,
                           MIR_new_mem_op (ctx, type,
                                           res.mir_op.u.mem.disp + (type == MIR_T_F ? 4 : 8) * i,
                                           res.mir_op.u.mem.base, res.mir_op.u.mem.index,
                                           res.mir_op.u.mem.scale));
      MIR_append_insn (ctx, curr_func, insn);
      VARR_PUSH (MIR_op_t, ret_ops, temp.mir_op);
    }
  } else if (ret_type->mode != TM_STRUCT && ret_type->mode != TM_UNION) {
    VARR_PUSH (MIR_op_t, ret_ops, res.mir_op);
  } else if (reg_aggregate_p (c2m_ctx, ret_type)) {
    size = type_size (c2m_ctx, ret_type);
    assert (res.mir_op.mode == MIR_OP_MEM && VARR_LENGTH (MIR_op_t, ret_ops) == 0);
    for (int i = 0; size > 0; size -= 8, i++)
      VARR_PUSH (MIR_op_t, ret_ops, get_new_temp (c2m_ctx, MIR_T_I64).mir_op);
    gen_multiple_load_store (c2m_ctx, ret_type, &VARR_ADDR (MIR_op_t, ret_ops)[0], res.mir_op,
                             TRUE);
  } else {
    ret_addr_reg = MIR_reg (ctx, RET_ADDR_NAME, curr_func->u.func);
    var = new_op (NULL, MIR_new_mem_op (ctx, MIR_T_I8, 0, ret_addr_reg, 0, 1));
    size = type_size (c2m_ctx, ret_type);
    block_move (c2m_ctx, var, res, size);
  }
}

static MIR_type_t target_get_blk_type (c2m_ctx_t c2m_ctx MIR_UNUSED,
                                       struct type *arg_type MIR_UNUSED) {
  return MIR_T_BLK; /* one BLK is enough */
}

static void target_add_arg_proto (c2m_ctx_t c2m_ctx, const char *name, struct type *arg_type,
                                  target_arg_info_t *arg_info MIR_UNUSED,
                                  VARR (MIR_var_t) * arg_vars) {
  MIR_var_t var;
  MIR_type_t type;
  int n;

  if (((type = fp_homogeneous_type (c2m_ctx, arg_type, &n)) == MIR_T_F || type == MIR_T_D)
      && n <= 8) {
    for (int i = 0; i < n; i++) {
      var.name = gen_get_indexed_name (c2m_ctx, name, i);
      var.type = type;
      VARR_PUSH (MIR_var_t, arg_vars, var);
    }
    return;
  }
  type = (arg_type->mode == TM_STRUCT || arg_type->mode == TM_UNION
            ? MIR_T_BLK
            : get_mir_type (c2m_ctx, arg_type));
  var.name = name;
  var.type = type;
  if (type == MIR_T_BLK) var.size = type_size (c2m_ctx, arg_type);
  VARR_PUSH (MIR_var_t, arg_vars, var);
}

static void target_add_call_arg_op (c2m_ctx_t c2m_ctx, struct type *arg_type,
                                    target_arg_info_t *arg_info MIR_UNUSED, op_t arg) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_type_t type;
  op_t temp;
  int n;

  if (((type = fp_homogeneous_type (c2m_ctx, arg_type, &n)) == MIR_T_F || type == MIR_T_D)
      && n <= 8) {
    assert (arg.mir_op.mode == MIR_OP_MEM);
    arg = mem_to_address (c2m_ctx, arg, TRUE);
    for (int i = 0; i < n; i++) {
      temp = get_new_temp (c2m_ctx, type);
      MIR_append_insn (ctx, curr_func,
                       MIR_new_insn (ctx, tp_mov (type), temp.mir_op,
                                     MIR_new_mem_op (ctx, type, (type == MIR_T_F ? 4 : 8) * i,
                                                     arg.mir_op.u.reg, 0, 1)));
      VARR_PUSH (MIR_op_t, call_ops, temp.mir_op);
    }
    return;
  }
  if (arg_type->mode != TM_STRUCT && arg_type->mode != TM_UNION) {
    VARR_PUSH (MIR_op_t, call_ops, arg.mir_op);
  } else {
    assert (arg.mir_op.mode == MIR_OP_MEM);
    arg = mem_to_address (c2m_ctx, arg, TRUE);
    VARR_PUSH (MIR_op_t, call_ops,
               MIR_new_mem_op (ctx, MIR_T_BLK, type_size (c2m_ctx, arg_type), arg.mir_op.u.reg, 0,
                               1));
  }
}

static int target_gen_gather_arg (c2m_ctx_t c2m_ctx, const char *name, struct type *arg_type,
                                  decl_t param_decl, target_arg_info_t *arg_info MIR_UNUSED) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_type_t type;
  reg_var_t reg_var;
  int i, n;

  if (((type = fp_homogeneous_type (c2m_ctx, arg_type, &n)) == MIR_T_F || type == MIR_T_D)
      && n <= 8) {
    for (i = 0; i < n; i++) {
      assert (!param_decl->reg_p);
      reg_var = get_reg_var (c2m_ctx, type, gen_get_indexed_name (c2m_ctx, name, i), NULL);
      MIR_append_insn (ctx, curr_func,
                       MIR_new_insn (ctx, tp_mov (type),
                                     MIR_new_mem_op (ctx, type,
                                                     param_decl->offset
                                                       + (type == MIR_T_F ? 4 : 8) * i,
                                                     MIR_reg (ctx, FP_NAME, curr_func->u.func), 0,
                                                     1),
                                     MIR_new_reg_op (ctx, reg_var.reg)));
    }
    return TRUE;
  }
  return FALSE;
}
