/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
   s390x call ABI target specific code.
*/

typedef int target_arg_info_t;

static void target_init_arg_vars (c2m_ctx_t c2m_ctx MIR_UNUSED,
                                  target_arg_info_t *arg_info MIR_UNUSED) {}

static int target_return_by_addr_p (c2m_ctx_t c2m_ctx, struct type *ret_type) {
  return simple_return_by_addr_p (c2m_ctx, ret_type);
}

static void target_add_res_proto (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                  target_arg_info_t *arg_info, VARR (MIR_type_t) * res_types,
                                  VARR (MIR_var_t) * arg_vars) {
  simple_add_res_proto (c2m_ctx, ret_type, arg_info, res_types, arg_vars);
}

static int target_add_call_res_op (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                   target_arg_info_t *arg_info, size_t call_arg_area_offset) {
  return simple_add_call_res_op (c2m_ctx, ret_type, arg_info, call_arg_area_offset);
}

static op_t target_gen_post_call_res_code (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res,
                                           MIR_insn_t call, size_t call_ops_start) {
  return simple_gen_post_call_res_code (c2m_ctx, ret_type, res, call, call_ops_start);
}

static void target_add_ret_ops (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res) {
  simple_add_ret_ops (c2m_ctx, ret_type, res);
}

static int reg_aggregate_p (c2m_ctx_t c2m_ctx, struct type *arg_type) {
  size_t size = type_size (c2m_ctx, arg_type);
  return size == 1 || size == 2 || size == 4 || size == 8;
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

  if (arg_type->mode != TM_STRUCT && arg_type->mode != TM_UNION)
    type = get_mir_type (c2m_ctx, arg_type);
  else if (reg_aggregate_p (c2m_ctx, arg_type))
    type = MIR_T_I64;
  else
    type = MIR_T_BLK;
  var.name = name;
  var.type = type;
  if (type == MIR_T_BLK) var.size = type_size (c2m_ctx, arg_type);
  VARR_PUSH (MIR_var_t, arg_vars, var);
}

static void target_add_call_arg_op (c2m_ctx_t c2m_ctx, struct type *arg_type,
                                    target_arg_info_t *arg_info MIR_UNUSED, op_t arg) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  op_t temp;

  if (arg_type->mode != TM_STRUCT && arg_type->mode != TM_UNION) {
    VARR_PUSH (MIR_op_t, call_ops, arg.mir_op);
  } else if (reg_aggregate_p (c2m_ctx, arg_type)) {
    assert (arg.mir_op.mode == MIR_OP_MEM);
    temp = get_new_temp (c2m_ctx, MIR_T_I64);
    gen_multiple_load_store (c2m_ctx, arg_type, &temp.mir_op, arg.mir_op, TRUE);
    VARR_PUSH (MIR_op_t, call_ops, temp.mir_op);
  } else {
    assert (arg.mir_op.mode == MIR_OP_MEM);
    arg = mem_to_address (c2m_ctx, arg, TRUE);
    VARR_PUSH (MIR_op_t, call_ops,
               MIR_new_mem_op (c2m_ctx->ctx, MIR_T_BLK, type_size (c2m_ctx, arg_type),
                               arg.mir_op.u.reg, 0, 1));
  }
}

static int target_gen_gather_arg (c2m_ctx_t c2m_ctx, const char *name, struct type *arg_type,
                                  decl_t param_decl, target_arg_info_t *arg_info MIR_UNUSED) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_op_t param_op;
  reg_var_t reg_var;

  if ((arg_type->mode != TM_STRUCT && arg_type->mode != TM_UNION)
      || !reg_aggregate_p (c2m_ctx, arg_type))
    return FALSE;
  assert (!param_decl->reg_p);
  reg_var = get_reg_var (c2m_ctx, MIR_T_I64, name, NULL);
  param_op = MIR_new_reg_op (ctx, reg_var.reg);
  gen_multiple_load_store (c2m_ctx, arg_type, &param_op,
                           MIR_new_mem_op (ctx, MIR_T_UNDEF, param_decl->offset,
                                           MIR_reg (ctx, FP_NAME, curr_func->u.func), 0, 1),
                           FALSE);
  return TRUE;
}
