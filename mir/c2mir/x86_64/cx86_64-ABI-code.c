/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
   x86_64 ABI target specific code.
*/

/* See https://gitlab.com/x86-psABIs/x86-64-ABI.
   We use MIR_T_UNDEF for MEMORY.
*/

enum add_arg_class { NO_CLASS = MIR_T_BOUND + 1, X87UP_CLASS };

#ifndef _WIN32
#define MAX_QWORDS 2
#else
#define MAX_QWORDS 1
#endif

static MIR_type_t get_result_type (MIR_type_t arg_type1, MIR_type_t arg_type2) {
  if (arg_type1 == arg_type2) return arg_type1;
  if ((enum add_arg_class) arg_type1 == NO_CLASS) return arg_type2;
  if ((enum add_arg_class) arg_type2 == NO_CLASS) return arg_type1;

  if (arg_type1 == MIR_T_UNDEF || arg_type2 == MIR_T_UNDEF) return MIR_T_UNDEF;

  if (arg_type1 == MIR_T_I64 || arg_type1 == MIR_T_I32 || arg_type2 == MIR_T_I64
      || arg_type2 == MIR_T_I32)
    return MIR_T_I64;

  if (arg_type1 == MIR_T_LD || arg_type2 == MIR_T_LD
      || (enum add_arg_class) arg_type1 == X87UP_CLASS
      || (enum add_arg_class) arg_type2 == X87UP_CLASS)
    return MIR_T_UNDEF;

  return MIR_T_D;
}

static int classify_arg (c2m_ctx_t c2m_ctx, struct type *type, MIR_type_t types[MAX_QWORDS],
                         int bit_field_p MIR_UNUSED) {
  size_t size = type_size (c2m_ctx, type), n_qwords = (size + 7) / 8;
  MIR_type_t mir_type;

  if (type->mode == TM_STRUCT || type->mode == TM_UNION || type->mode == TM_ARR) {
    if (n_qwords > MAX_QWORDS) return 0; /* too big aggregate */

#ifndef _WIN32
    MIR_type_t subtypes[MAX_QWORDS];
    int i, n_el_qwords;
    for (i = 0; (size_t) i < n_qwords; i++) types[i] = (MIR_type_t) NO_CLASS;

    switch (type->mode) {
    case TM_ARR: { /* Arrays are handled as small records.  */
      n_el_qwords = classify_arg (c2m_ctx, type->u.arr_type->el_type, subtypes, FALSE);
      if (n_el_qwords == 0) return 0;
      /* make full types: */
      for (i = 0; (size_t) i < n_qwords; i++)
        types[i] = get_result_type (types[i], subtypes[i % n_el_qwords]);
      break;
    }
    case TM_STRUCT:
    case TM_UNION:
      for (node_t el = NL_HEAD (NL_EL (type->u.tag_type->u.ops, 1)->u.ops); el != NULL;
           el = NL_NEXT (el))
        if (el->code == N_MEMBER) {
          decl_t decl = el->attr;
          mir_size_t offset = decl->offset;
          node_t container;
          if ((container = decl->containing_unnamed_anon_struct_union_member) != NULL) {
            decl_t decl2 = container->attr;
            assert (decl2->decl_spec.type->mode == TM_STRUCT
                    || decl2->decl_spec.type->mode == TM_UNION);
            offset -= decl2->offset;
          }
          int start_qword = offset / 8;
          int end_qword = (offset + type_size (c2m_ctx, decl->decl_spec.type) - 1) / 8;
          int span_qwords = end_qword - start_qword + 1;

          if (decl->bit_offset >= 0) {
            types[start_qword] = get_result_type (MIR_T_I64, types[start_qword]);
          } else {
            n_el_qwords
              = classify_arg (c2m_ctx, decl->decl_spec.type, subtypes, decl->bit_offset >= 0);
            if (n_el_qwords == 0) return 0;
            for (i = 0; i < n_el_qwords && (size_t) (i + start_qword) < n_qwords; i++) {
              types[i + start_qword] = get_result_type (subtypes[i], types[i + start_qword]);
              if (span_qwords > n_el_qwords)
                types[i + start_qword + 1]
                  = get_result_type (subtypes[i], types[i + start_qword + 1]);
            }
          }
        }
      break;
    default: assert (FALSE);
    }

    if (n_qwords > 2) return 0; /* as we don't have vector values (see SSEUP_CLASS) */

    for (i = 0; (size_t) i < n_qwords; i++) {
      if (types[i] == MIR_T_UNDEF) return 0; /* pass in memory if a word class is memory.  */
      if ((enum add_arg_class) types[i] == X87UP_CLASS && (i == 0 || types[i - 1] != MIR_T_LD))
        return 0;
    }
    return n_qwords;
#else
    types[0] = MIR_T_I64;
    return 1;
#endif
  }

  assert (scalar_type_p (type));
  switch (mir_type = get_mir_type (c2m_ctx, type)) {
  case MIR_T_F:
  case MIR_T_D: types[0] = MIR_T_D; return 1;
  case MIR_T_LD:
    types[0] = MIR_T_LD;
    types[1] = (MIR_type_t) X87UP_CLASS;
    return 2;
  default: types[0] = MIR_T_I64; return 1;
  }
}

typedef struct target_arg_info {
  int n_iregs, n_fregs;
} target_arg_info_t;

static void target_init_arg_vars (c2m_ctx_t c2m_ctx MIR_UNUSED, target_arg_info_t *arg_info) {
  arg_info->n_iregs = arg_info->n_fregs = 0;
}

static void update_last_qword_type (c2m_ctx_t c2m_ctx, struct type *type,
                                    MIR_type_t qword_types[MAX_QWORDS], int n) {
  size_t last_size, size = type_size (c2m_ctx, type);
  MIR_type_t mir_type;

  assert (n != 0);
  if ((last_size = size % 8) == 0 || n > 1) return;
  mir_type = qword_types[n - 1];
  if (last_size <= 4 && mir_type == MIR_T_D) qword_types[n - 1] = MIR_T_F;
  if (last_size <= 4 && mir_type == MIR_T_I64)
    qword_types[n - 1] = last_size <= 1 ? MIR_T_I8 : last_size <= 2 ? MIR_T_I16 : MIR_T_I32;
}

static int process_ret_type (c2m_ctx_t c2m_ctx, struct type *ret_type,
                             MIR_type_t qword_types[MAX_QWORDS]) {
  MIR_type_t type;
  int n, n_iregs, n_fregs, n_stregs, curr;
  int n_qwords = classify_arg (c2m_ctx, ret_type, qword_types, FALSE);

  if (ret_type->mode != TM_STRUCT && ret_type->mode != TM_UNION) return 0;
  if (n_qwords != 0) {
    update_last_qword_type (c2m_ctx, ret_type, qword_types, n_qwords);
    n_iregs = n_fregs = n_stregs = curr = 0;
    for (n = 0; n < n_qwords; n++) { /* start from the last qword */
      type = qword_types[n];
      qword_types[curr++] = type;
      switch ((int) type) {
      case MIR_T_I8:
      case MIR_T_I16:
      case MIR_T_I32:
      case MIR_T_I64: n_iregs++; break;
      case MIR_T_F:
      case MIR_T_D: n_fregs++; break;
      case MIR_T_LD: n_stregs++; break;
      case X87UP_CLASS:
        n_qwords--;
        curr--;
        break;
      default: assert (FALSE);
      }
    }
    if (n_iregs > 2 || n_fregs > 2 || n_stregs > 1) n_qwords = 0;
  }
  return n_qwords;
}

static int target_return_by_addr_p (c2m_ctx_t c2m_ctx, struct type *ret_type) {
  MIR_type_t qword_types[MAX_QWORDS];
  int n_qwords;

  if (void_type_p (ret_type)) return FALSE;
  n_qwords = process_ret_type (c2m_ctx, ret_type, qword_types);
  return n_qwords == 0 && (ret_type->mode == TM_STRUCT || ret_type->mode == TM_UNION);
}

static void target_add_res_proto (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                  target_arg_info_t *arg_info, VARR (MIR_type_t) * res_types,
                                  VARR (MIR_var_t) * arg_vars) {
  MIR_var_t var;
  MIR_type_t type;
  MIR_type_t qword_types[MAX_QWORDS];
  int n, n_qwords;

  if (void_type_p (ret_type)) return;
  n_qwords = process_ret_type (c2m_ctx, ret_type, qword_types);
  if (n_qwords != 0) {
    for (n = 0; n < n_qwords; n++)
      VARR_PUSH (MIR_type_t, res_types, promote_mir_int_type (qword_types[n]));
  } else if (ret_type->mode != TM_STRUCT && ret_type->mode != TM_UNION) {
    type = get_mir_type (c2m_ctx, ret_type);
    VARR_PUSH (MIR_type_t, res_types, type);
  } else { /* return by reference */
    var.name = RET_ADDR_NAME;
    var.type = MIR_T_RBLK;
    var.size = type_size (c2m_ctx, ret_type);
    VARR_PUSH (MIR_var_t, arg_vars, var);
    arg_info->n_iregs++;
  }
}

static int target_add_call_res_op (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                   target_arg_info_t *arg_info, size_t call_arg_area_offset) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_type_t type;
  MIR_type_t qword_types[MAX_QWORDS];
  op_t temp;
  int i, n_qwords;

  if (void_type_p (ret_type)) return -1;
  n_qwords = process_ret_type (c2m_ctx, ret_type, qword_types);
  if (n_qwords != 0) {
    for (i = 0; i < n_qwords; i++) {
      temp = get_new_temp (c2m_ctx, promote_mir_int_type (qword_types[i]));
      VARR_PUSH (MIR_op_t, call_ops, temp.mir_op);
    }
    return n_qwords;
  } else if (ret_type->mode == TM_STRUCT || ret_type->mode == TM_UNION) { /* return by reference */
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

static op_t target_gen_post_call_res_code (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res,
                                           MIR_insn_t call MIR_UNUSED, size_t call_ops_start) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_type_t type;
  MIR_insn_t insn;
  MIR_type_t qword_types[MAX_QWORDS];
  int i, n_qwords;

  if (void_type_p (ret_type)) return res;
  n_qwords = process_ret_type (c2m_ctx, ret_type, qword_types);
  if (n_qwords != 0) {
    assert (res.mir_op.mode == MIR_OP_MEM);
    for (i = 0; i < n_qwords; i++) {
      type = qword_types[i];
      insn = MIR_new_insn (ctx, tp_mov (type),
                           MIR_new_mem_op (ctx, type, res.mir_op.u.mem.disp + 8 * i,
                                           res.mir_op.u.mem.base, res.mir_op.u.mem.index,
                                           res.mir_op.u.mem.scale),
                           VARR_GET (MIR_op_t, call_ops, i + call_ops_start + 2));
      MIR_append_insn (ctx, curr_func, insn);
    }
  }
  return res;
}

static void target_add_ret_ops (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_type_t type;
  MIR_type_t qword_types[MAX_QWORDS];
  MIR_insn_t insn;
  MIR_reg_t ret_addr_reg;
  op_t temp, var;
  int i, n_qwords;

  if (void_type_p (ret_type)) return;
  n_qwords = process_ret_type (c2m_ctx, ret_type, qword_types);
  if (n_qwords != 0) {
    for (i = 0; i < n_qwords; i++) {
      type = qword_types[i];
      temp = get_new_temp (c2m_ctx, promote_mir_int_type (type));
      insn = MIR_new_insn (ctx, tp_mov (type), temp.mir_op,
                           MIR_new_mem_op (ctx, type, res.mir_op.u.mem.disp + 8 * i,
                                           res.mir_op.u.mem.base, res.mir_op.u.mem.index,
                                           res.mir_op.u.mem.scale));
      MIR_append_insn (ctx, curr_func, insn);
      VARR_PUSH (MIR_op_t, ret_ops, temp.mir_op);
    }
  } else if (ret_type->mode != TM_STRUCT && ret_type->mode != TM_UNION) {
    VARR_PUSH (MIR_op_t, ret_ops, res.mir_op);
  } else {
    ret_addr_reg = MIR_reg (ctx, RET_ADDR_NAME, curr_func->u.func);
    var = new_op (NULL, MIR_new_mem_op (ctx, MIR_T_I8, 0, ret_addr_reg, 0, 1));
    block_move (c2m_ctx, var, res, type_size (c2m_ctx, ret_type));
  }
}

static int process_aggregate_arg (c2m_ctx_t c2m_ctx, struct type *arg_type,
                                  target_arg_info_t *arg_info, MIR_type_t qword_types[MAX_QWORDS]) {
  MIR_type_t type;
  int n, n_iregs, n_fregs, n_qwords = classify_arg (c2m_ctx, arg_type, qword_types, FALSE);

  if (n_qwords == 0) return 0;
  if (arg_type->mode != TM_STRUCT && arg_type->mode != TM_UNION) return 0;
  update_last_qword_type (c2m_ctx, arg_type, qword_types, n_qwords);
  n_iregs = n_fregs = 0;
  for (n = 0; n < n_qwords; n++) { /* start from the last qword */
    switch ((int) (type = qword_types[n])) {
    case MIR_T_I8:
    case MIR_T_I16:
    case MIR_T_I32:
    case MIR_T_I64: n_iregs++; break;
    case MIR_T_F:
    case MIR_T_D: n_fregs++; break;
    case X87UP_CLASS:
    case MIR_T_LD: return 0;
    default: assert (FALSE);
    }
  }
  if (arg_info->n_iregs + n_iregs > 6 || arg_info->n_fregs + n_fregs > 8) return 0;
  /* aggregate passed by value: update arg_info */
  arg_info->n_iregs += n_iregs;
  arg_info->n_fregs += n_fregs;
  return n_qwords;
}

static MIR_type_t get_blk_type (int n_qwords, MIR_type_t *qword_types) {
  int n, n_iregs = 0, n_fregs = 0;

  assert (n_qwords <= 2);
  if (n_qwords == 0) return MIR_T_BLK;
  for (n = 0; n < n_qwords; n++) { /* start from the last qword */
    switch ((int) qword_types[n]) {
    case MIR_T_I8:
    case MIR_T_I16:
    case MIR_T_I32:
    case MIR_T_I64: n_iregs++; break;
    case MIR_T_F:
    case MIR_T_D: n_fregs++; break;
    case X87UP_CLASS:
    case MIR_T_LD: return MIR_T_BLK;
    default: assert (FALSE);
    }
  }
  if (n_iregs == n_qwords) return MIR_T_BLK + 1;
  if (n_fregs == n_qwords) return MIR_T_BLK + 2;
  if (qword_types[0] == MIR_T_F || qword_types[0] == MIR_T_D) return MIR_T_BLK + 4;
  return MIR_T_BLK + 3;
}

static MIR_type_t target_get_blk_type (c2m_ctx_t c2m_ctx, struct type *arg_type) {
  MIR_type_t qword_types[MAX_QWORDS];
  int n_qwords = classify_arg (c2m_ctx, arg_type, qword_types, FALSE);
  assert (arg_type->mode == TM_STRUCT || arg_type->mode == TM_UNION);
  return get_blk_type (n_qwords, qword_types);
}

static void target_add_arg_proto (c2m_ctx_t c2m_ctx, const char *name, struct type *arg_type,
                                  target_arg_info_t *arg_info, VARR (MIR_var_t) * arg_vars) {
  MIR_var_t var;
  MIR_type_t type;
  MIR_type_t qword_types[MAX_QWORDS];
  int n_qwords = process_aggregate_arg (c2m_ctx, arg_type, arg_info, qword_types);

  /* pass aggregates on the stack and pass by value for others: */
  var.name = name;
  if (arg_type->mode != TM_STRUCT && arg_type->mode != TM_UNION) {
    type = get_mir_type (c2m_ctx, arg_type);
    var.type = type;
    if (type == MIR_T_F || type == MIR_T_D)
      arg_info->n_fregs++;
    else if (type != MIR_T_LD)
      arg_info->n_iregs++;
  } else {
    var.type = get_blk_type (n_qwords, qword_types);
    var.size = type_size (c2m_ctx, arg_type);
  }
  VARR_PUSH (MIR_var_t, arg_vars, var);
}

static void target_add_call_arg_op (c2m_ctx_t c2m_ctx, struct type *arg_type,
                                    target_arg_info_t *arg_info, op_t arg) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_type_t type;
  MIR_type_t qword_types[MAX_QWORDS];
  int n_qwords = process_aggregate_arg (c2m_ctx, arg_type, arg_info, qword_types);

  /* pass aggregates on the stack and pass by value for others: */
  if (arg_type->mode != TM_STRUCT && arg_type->mode != TM_UNION) {
    type = get_mir_type (c2m_ctx, arg_type);
    VARR_PUSH (MIR_op_t, call_ops, arg.mir_op);
    if (type == MIR_T_F || type == MIR_T_D)
      arg_info->n_fregs++;
    else if (type != MIR_T_LD)
      arg_info->n_iregs++;
  } else {
    assert (arg.mir_op.mode == MIR_OP_MEM);
    arg = mem_to_address (c2m_ctx, arg, TRUE);
    type = get_blk_type (n_qwords, qword_types);
    VARR_PUSH (MIR_op_t, call_ops,
               MIR_new_mem_op (ctx, type, type_size (c2m_ctx, arg_type), arg.mir_op.u.reg, 0, 1));
  }
}

static int target_gen_gather_arg (c2m_ctx_t c2m_ctx MIR_UNUSED, const char *name MIR_UNUSED,
                                  struct type *arg_type MIR_UNUSED, decl_t param_decl MIR_UNUSED,
                                  target_arg_info_t *arg_info MIR_UNUSED) {
  return FALSE;
}
