/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
   aarch64 call ABI target specific code.
*/

typedef int target_arg_info_t;

static void target_init_arg_vars (c2m_ctx_t c2m_ctx MIR_UNUSED,
                                  target_arg_info_t *arg_info MIR_UNUSED) {}

static int target_return_by_addr_p (c2m_ctx_t c2m_ctx, struct type *ret_type) {
  return ((ret_type->mode == TM_STRUCT || ret_type->mode == TM_UNION)
          && type_size (c2m_ctx, ret_type) > 2 * 8);
}

static int reg_aggregate_size (c2m_ctx_t c2m_ctx, struct type *type) {
  size_t size;

  if (type->mode != TM_STRUCT && type->mode != TM_UNION) return -1;
  return (size = type_size (c2m_ctx, type)) <= 2 * 8 ? (int) size : -1;
}

static void target_add_res_proto (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                  target_arg_info_t *arg_info, VARR (MIR_type_t) * res_types,
                                  VARR (MIR_var_t) * arg_vars) {
  int size;

  if ((size = reg_aggregate_size (c2m_ctx, ret_type)) < 0) {
    simple_add_res_proto (c2m_ctx, ret_type, arg_info, res_types, arg_vars);
    return;
  }
  if (size == 0) return;
  VARR_PUSH (MIR_type_t, res_types, MIR_T_I64);
  if (size > 8) VARR_PUSH (MIR_type_t, res_types, MIR_T_I64);
}

static int target_add_call_res_op (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                   target_arg_info_t *arg_info, size_t call_arg_area_offset) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  int size;

  if ((size = reg_aggregate_size (c2m_ctx, ret_type)) < 0)
    return simple_add_call_res_op (c2m_ctx, ret_type, arg_info, call_arg_area_offset);
  if (size == 0) return -1;
  VARR_PUSH (MIR_op_t, call_ops,
             MIR_new_reg_op (ctx, get_new_temp (c2m_ctx, MIR_T_I64).mir_op.u.reg));
  if (size > 8)
    VARR_PUSH (MIR_op_t, call_ops,
               MIR_new_reg_op (ctx, get_new_temp (c2m_ctx, MIR_T_I64).mir_op.u.reg));
  return size <= 8 ? 1 : 2;
}

static op_t target_gen_post_call_res_code (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res,
                                           MIR_insn_t call, size_t call_ops_start) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  int size;

  if ((size = reg_aggregate_size (c2m_ctx, ret_type)) < 0)
    return simple_gen_post_call_res_code (c2m_ctx, ret_type, res, call, call_ops_start);
  if (size != 0)
    gen_multiple_load_store (c2m_ctx, ret_type, &VARR_ADDR (MIR_op_t, call_ops)[call_ops_start + 2],
                             res.mir_op, FALSE);
  return res;
}

static void target_add_ret_ops (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  int i, size;

  if ((size = reg_aggregate_size (c2m_ctx, ret_type)) < 0) {
    simple_add_ret_ops (c2m_ctx, ret_type, res);
    return;
  }
  assert (res.mir_op.mode == MIR_OP_MEM && VARR_LENGTH (MIR_op_t, ret_ops) == 0 && size <= 2 * 8);
  for (i = 0; size > 0; size -= 8, i++)
    VARR_PUSH (MIR_op_t, ret_ops, get_new_temp (c2m_ctx, MIR_T_I64).mir_op);
  gen_multiple_load_store (c2m_ctx, ret_type, VARR_ADDR (MIR_op_t, ret_ops), res.mir_op, TRUE);
}

static MIR_type_t target_get_blk_type (c2m_ctx_t c2m_ctx MIR_UNUSED,
                                       struct type *arg_type MIR_UNUSED) {
  return MIR_T_BLK; /* one BLK is enough */
}

static void target_add_arg_proto (c2m_ctx_t c2m_ctx, const char *name, struct type *arg_type,
                                  target_arg_info_t *arg_info, VARR (MIR_var_t) * arg_vars) {
  simple_add_arg_proto (c2m_ctx, name, arg_type, arg_info, arg_vars);
}

static void target_add_call_arg_op (c2m_ctx_t c2m_ctx, struct type *arg_type,
                                    target_arg_info_t *arg_info, op_t arg) {
  simple_add_call_arg_op (c2m_ctx, arg_type, arg_info, arg);
}

static int target_gen_gather_arg (c2m_ctx_t c2m_ctx MIR_UNUSED, const char *name MIR_UNUSED,
                                  struct type *arg_type MIR_UNUSED, decl_t param_decl MIR_UNUSED,
                                  target_arg_info_t *arg_info MIR_UNUSED) {
  return FALSE;
}
