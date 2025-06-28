static MIR_item_t create_mir_func_sieve_api (MIR_context_t ctx, MIR_module_t *m_res) {
  MIR_item_t func;
  MIR_module_t m;
  MIR_reg_t iter, count, i, k, prime, flags;
  MIR_type_t res_type;
  MIR_label_t loop = MIR_new_label (ctx), loop2 = MIR_new_label (ctx), loop3 = MIR_new_label (ctx),
              loop4 = MIR_new_label (ctx);
  MIR_label_t fin = MIR_new_label (ctx), fin2 = MIR_new_label (ctx), fin3 = MIR_new_label (ctx),
              fin4 = MIR_new_label (ctx);
  MIR_label_t cont3 = MIR_new_label (ctx);

  m = MIR_new_module (ctx, "m_sieve");
  if (m_res != NULL) *m_res = m;
  res_type = MIR_T_I64;
  func = MIR_new_func (ctx, "sieve", 1, &res_type, 0);
  iter = MIR_new_func_reg (ctx, func->u.func, MIR_T_I64, "iter");
  count = MIR_new_func_reg (ctx, func->u.func, MIR_T_I64, "count");
  i = MIR_new_func_reg (ctx, func->u.func, MIR_T_I64, "i");
  k = MIR_new_func_reg (ctx, func->u.func, MIR_T_I64, "k");
  prime = MIR_new_func_reg (ctx, func->u.func, MIR_T_I64, "prime");
  flags = MIR_new_func_reg (ctx, func->u.func, MIR_T_I64, "flags");
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_ALLOCA, MIR_new_reg_op (ctx, flags),
                                 MIR_new_int_op (ctx, 819000)));
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_MOV, MIR_new_reg_op (ctx, iter),
                                 MIR_new_int_op (ctx, 0)));
  MIR_append_insn (ctx, func, loop);
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_BGE, MIR_new_label_op (ctx, fin),
                                 MIR_new_reg_op (ctx, iter), MIR_new_int_op (ctx, 100)));
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_MOV, MIR_new_reg_op (ctx, count),
                                 MIR_new_int_op (ctx, 0)));
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_MOV, MIR_new_reg_op (ctx, i), MIR_new_int_op (ctx, 0)));
  MIR_append_insn (ctx, func, loop2);
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_BGE, MIR_new_label_op (ctx, fin2),
                                 MIR_new_reg_op (ctx, i), MIR_new_int_op (ctx, 819000)));
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_MOV, MIR_new_mem_op (ctx, MIR_T_U8, 0, flags, i, 1),
                                 MIR_new_int_op (ctx, 1)));
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_ADD, MIR_new_reg_op (ctx, i), MIR_new_reg_op (ctx, i),
                                 MIR_new_int_op (ctx, 1)));
  MIR_append_insn (ctx, func, MIR_new_insn (ctx, MIR_JMP, MIR_new_label_op (ctx, loop2)));
  MIR_append_insn (ctx, func, fin2);
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_MOV, MIR_new_reg_op (ctx, i), MIR_new_int_op (ctx, 1)));
  MIR_append_insn (ctx, func, loop3);
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_BGE, MIR_new_label_op (ctx, fin3),
                                 MIR_new_reg_op (ctx, i), MIR_new_int_op (ctx, 819000)));
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_BEQ, MIR_new_label_op (ctx, cont3),
                                 MIR_new_mem_op (ctx, MIR_T_U8, 0, flags, i, 1),
                                 MIR_new_int_op (ctx, 0)));
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_ADD, MIR_new_reg_op (ctx, prime), MIR_new_reg_op (ctx, i),
                                 MIR_new_int_op (ctx, 1)));
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_ADD, MIR_new_reg_op (ctx, k), MIR_new_reg_op (ctx, i),
                                 MIR_new_reg_op (ctx, prime)));
  MIR_append_insn (ctx, func, loop4);
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_BGE, MIR_new_label_op (ctx, fin4),
                                 MIR_new_reg_op (ctx, k), MIR_new_int_op (ctx, 819000)));
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_MOV, MIR_new_mem_op (ctx, MIR_T_U8, 0, flags, k, 1),
                                 MIR_new_int_op (ctx, 0)));
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_ADD, MIR_new_reg_op (ctx, k), MIR_new_reg_op (ctx, k),
                                 MIR_new_reg_op (ctx, prime)));
  MIR_append_insn (ctx, func, MIR_new_insn (ctx, MIR_JMP, MIR_new_label_op (ctx, loop4)));
  MIR_append_insn (ctx, func, fin4);
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_ADD, MIR_new_reg_op (ctx, count),
                                 MIR_new_reg_op (ctx, count), MIR_new_int_op (ctx, 1)));
  MIR_append_insn (ctx, func, cont3);
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_ADD, MIR_new_reg_op (ctx, i), MIR_new_reg_op (ctx, i),
                                 MIR_new_int_op (ctx, 1)));
  MIR_append_insn (ctx, func, MIR_new_insn (ctx, MIR_JMP, MIR_new_label_op (ctx, loop3)));
  MIR_append_insn (ctx, func, fin3);
  MIR_append_insn (ctx, func,
                   MIR_new_insn (ctx, MIR_ADD, MIR_new_reg_op (ctx, iter),
                                 MIR_new_reg_op (ctx, iter), MIR_new_int_op (ctx, 1)));
  MIR_append_insn (ctx, func, MIR_new_insn (ctx, MIR_JMP, MIR_new_label_op (ctx, loop)));
  MIR_append_insn (ctx, func, fin);
  MIR_append_insn (ctx, func, MIR_new_ret_insn (ctx, 1, MIR_new_reg_op (ctx, count)));
  MIR_finish_func (ctx);
  MIR_finish_module (ctx);
  return func;
}
