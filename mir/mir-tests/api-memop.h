MIR_item_t create_mir_example2 (MIR_context_t ctx, MIR_module_t *m) {
  MIR_item_t func;
  MIR_reg_t ARG1, ARG2;
  MIR_type_t res_type;

  if (m != NULL) *m = MIR_new_module (ctx, "m");
  res_type = MIR_T_I64;
  func = MIR_new_func (ctx, "memop", 1, &res_type, 2, MIR_T_I64, "arg1", MIR_T_I64, "arg2");
  ARG1 = MIR_reg (ctx, "arg1", func->u.func);
  ARG2 = MIR_reg (ctx, "arg2", func->u.func);
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_ADD, MIR_new_mem_op (ctx, MIR_T_I64, 0, ARG1, ARG2, 8),
                                 MIR_new_mem_op (ctx, MIR_T_I64, 64, ARG1, 0, 0),
                                 MIR_new_mem_op (ctx, MIR_T_I64, 0, 0, ARG1, 8)));
  MIR_append_insn (ctx, func,
                   MIR_new_ret_insn (ctx, 1, MIR_new_mem_op (ctx, MIR_T_I64, 0, ARG1, 0, 0)));
  MIR_append_insn (ctx, func,
                   MIR_new_ret_insn (ctx, 1, MIR_new_mem_op (ctx, MIR_T_I64, 0, 0, ARG2, 1)));
  MIR_append_insn (ctx, func,
                   MIR_new_ret_insn (ctx, 1, MIR_new_mem_op (ctx, MIR_T_I64, 1024, 0, 0, 0)));
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_MOV, MIR_new_mem_op (ctx, MIR_T_I64, 0, ARG1, ARG2, 8),
                                 MIR_new_mem_op (ctx, MIR_T_I64, 0, ARG1, 0, 8)));
  MIR_finish_func (ctx);
  if (m != NULL) MIR_finish_module (ctx);
  return func;
}
