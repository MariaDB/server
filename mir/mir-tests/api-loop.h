MIR_item_t create_mir_func_with_loop (MIR_context_t ctx, MIR_module_t *m) {
  MIR_item_t func;
  MIR_label_t fin, cont;
  MIR_reg_t ARG1, R2;
  MIR_type_t res_type;

  if (m != NULL) *m = MIR_new_module (ctx, "m");
  res_type = MIR_T_I64;
  func = MIR_new_func (ctx, "loop", 1, &res_type, 1, MIR_T_I64, "arg1");
  R2 = MIR_new_func_reg (ctx, func->u.func, MIR_T_I64, "count");
  ARG1 = MIR_reg (ctx, "arg1", func->u.func);
  fin = MIR_new_label (ctx);
  cont = MIR_new_label (ctx);
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_MOV, MIR_new_reg_op (ctx, R2), MIR_new_int_op (ctx, 0)));
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_BGE, MIR_new_label_op (ctx, fin),
                                 MIR_new_reg_op (ctx, R2), MIR_new_reg_op (ctx, ARG1)));
  MIR_append_insn (ctx, func, cont);
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_ADD, MIR_new_reg_op (ctx, R2), MIR_new_reg_op (ctx, R2),
                                 MIR_new_int_op (ctx, 1)));
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_BLT, MIR_new_label_op (ctx, cont),
                                 MIR_new_reg_op (ctx, R2), MIR_new_reg_op (ctx, ARG1)));
  MIR_append_insn (ctx, func, fin);
  MIR_append_insn (ctx, func, MIR_new_ret_insn (ctx, 1, MIR_new_reg_op (ctx, R2)));
  MIR_finish_func (ctx);
  if (m != NULL) MIR_finish_module (ctx);
  return func;
}
