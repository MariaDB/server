/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include "mir2c.h"
#include <float.h>
#include <inttypes.h>

static MIR_func_t curr_func;
static int curr_temp;

static void out_type (FILE *f, MIR_type_t t) {
  switch (t) {
  case MIR_T_I8: fprintf (f, "int8_t"); break;
  case MIR_T_U8: fprintf (f, "uint8_t"); break;
  case MIR_T_I16: fprintf (f, "int16_t"); break;
  case MIR_T_U16: fprintf (f, "uint16_t"); break;
  case MIR_T_I32: fprintf (f, "int32_t"); break;
  case MIR_T_U32: fprintf (f, "uint32_t"); break;
  case MIR_T_I64: fprintf (f, "int64_t"); break;
  case MIR_T_U64: fprintf (f, "uint64_t"); break;
  case MIR_T_F: fprintf (f, "float"); break;
  case MIR_T_D: fprintf (f, "double"); break;
  case MIR_T_LD: fprintf (f, "long double"); break;
  case MIR_T_P: fprintf (f, "void *"); break;
  default:
    mir_assert (MIR_blk_type_p (t));
    fprintf (f, "void *");
    break;
  }
}

static void out_op (MIR_context_t ctx, FILE *f, MIR_op_t op) {
  switch (op.mode) {
  case MIR_OP_REG: fprintf (f, "%s", MIR_reg_name (ctx, op.u.reg, curr_func)); break;
  case MIR_OP_INT: fprintf (f, "%" PRId64, op.u.i); break;
  case MIR_OP_UINT: fprintf (f, "%" PRIu64, op.u.u); break;
  case MIR_OP_FLOAT: fprintf (f, "%#.*gf", FLT_MANT_DIG, op.u.f); break;
  case MIR_OP_DOUBLE: fprintf (f, "%#.*g", DBL_MANT_DIG, op.u.d); break;
  case MIR_OP_LDOUBLE: fprintf (f, "%#.*lgl", LDBL_MANT_DIG, op.u.d); break;
  case MIR_OP_REF: fprintf (f, "%s", MIR_item_name (ctx, op.u.ref)); break;
  case MIR_OP_MEM: {
    MIR_reg_t no_reg = 0;
    int disp_p = FALSE, blk_p = MIR_blk_type_p (op.u.mem.type);

    if (!blk_p) {
      fprintf (f, "*(");
      out_type (f, op.u.mem.type);
      fprintf (f, "*) ");
    }
    fprintf (f, "(");
    if (op.u.mem.disp != 0 || (op.u.mem.base == no_reg && op.u.mem.index == no_reg)) {
      fprintf (f, "%" PRId64, blk_p ? 0 : op.u.mem.disp);
      disp_p = TRUE;
    }
    if (op.u.mem.base != no_reg || op.u.mem.index != no_reg) {
      if (disp_p) fprintf (f, " + ");
      if (op.u.mem.base != no_reg) fprintf (f, "%s", MIR_reg_name (ctx, op.u.mem.base, curr_func));
      if (op.u.mem.index != no_reg) {
        if (op.u.mem.base != no_reg) fprintf (f, " + ");
        fprintf (f, "%s", MIR_reg_name (ctx, op.u.mem.index, curr_func));
        if (op.u.mem.scale != 1) fprintf (f, " * %u", op.u.mem.scale);
      }
    }
    fprintf (f, ")");
    break;
  }
  case MIR_OP_LABEL:
    mir_assert (op.u.label->ops[0].mode == MIR_OP_INT);
    fprintf (f, "l%" PRId64, op.u.label->ops[0].u.i);
    break;
  case MIR_OP_STR: MIR_output_str (ctx, f, op.u.str); break;
  default: mir_assert (FALSE);
  }
}

static void out_op2 (MIR_context_t ctx, FILE *f, MIR_op_t *ops, const char *str) {
  out_op (ctx, f, ops[0]);
  fprintf (f, " = ");
  if (str != NULL) fprintf (f, "%s ", str);
  out_op (ctx, f, ops[1]);
  fprintf (f, ";\n");
}

static void out_op3 (MIR_context_t ctx, FILE *f, MIR_op_t *ops, const char *str) {
  out_op (ctx, f, ops[0]);
  fprintf (f, " = (int64_t) ");
  out_op (ctx, f, ops[1]);
  fprintf (f, " %s (int64_t) ", str);
  out_op (ctx, f, ops[2]);
  fprintf (f, ";\n");
}

static void out_uop3 (MIR_context_t ctx, FILE *f, MIR_op_t *ops, const char *str) {
  out_op (ctx, f, ops[0]);
  fprintf (f, " = (uint64_t) ");
  out_op (ctx, f, ops[1]);
  fprintf (f, " %s (uint64_t) ", str);
  out_op (ctx, f, ops[2]);
  fprintf (f, ";\n");
}

static void out_sop3 (MIR_context_t ctx, FILE *f, MIR_op_t *ops, const char *str) {
  out_op (ctx, f, ops[0]);
  fprintf (f, " = (int32_t) ");
  out_op (ctx, f, ops[1]);
  fprintf (f, " %s (int32_t) ", str);
  out_op (ctx, f, ops[2]);
  fprintf (f, ";\n");
}

static void out_usop3 (MIR_context_t ctx, FILE *f, MIR_op_t *ops, const char *str) {
  out_op (ctx, f, ops[0]);
  fprintf (f, " = (uint32_t) ");
  out_op (ctx, f, ops[1]);
  fprintf (f, " %s (uint32_t) ", str);
  out_op (ctx, f, ops[2]);
  fprintf (f, ";\n");
}

static void out_jmp (MIR_context_t ctx, FILE *f, MIR_op_t label_op) {
  mir_assert (label_op.mode == MIR_OP_LABEL);
  fprintf (f, "goto ");
  out_op (ctx, f, label_op);
  fprintf (f, ";\n");
}

static void out_bcmp (MIR_context_t ctx, FILE *f, MIR_op_t *ops, const char *str) {
  fprintf (f, "if ((int64_t) ");
  out_op (ctx, f, ops[1]);
  fprintf (f, " %s (int64_t) ", str);
  out_op (ctx, f, ops[2]);
  fprintf (f, ") ");
  out_jmp (ctx, f, ops[0]);
}

static void out_bucmp (MIR_context_t ctx, FILE *f, MIR_op_t *ops, const char *str) {
  fprintf (f, "if ((uint64_t) ");
  out_op (ctx, f, ops[1]);
  fprintf (f, " %s (uint64_t) ", str);
  out_op (ctx, f, ops[2]);
  fprintf (f, ") ");
  out_jmp (ctx, f, ops[0]);
}

static void out_bscmp (MIR_context_t ctx, FILE *f, MIR_op_t *ops, const char *str) {
  fprintf (f, "if ((int32_t) ");
  out_op (ctx, f, ops[1]);
  fprintf (f, " %s (int32_t) ", str);
  out_op (ctx, f, ops[2]);
  fprintf (f, ") ");
  out_jmp (ctx, f, ops[0]);
}

static void out_buscmp (MIR_context_t ctx, FILE *f, MIR_op_t *ops, const char *str) {
  fprintf (f, "if ((uint32_t) ");
  out_op (ctx, f, ops[1]);
  fprintf (f, " %s (uint32_t) ", str);
  out_op (ctx, f, ops[2]);
  fprintf (f, ") ");
  out_jmp (ctx, f, ops[0]);
}

static void out_fop3 (MIR_context_t ctx, FILE *f, MIR_op_t *ops, const char *str) {
  out_op (ctx, f, ops[0]);
  fprintf (f, " = ");
  out_op (ctx, f, ops[1]);
  fprintf (f, " %s ", str);
  out_op (ctx, f, ops[2]);
  fprintf (f, ";\n");
}

static void out_bfcmp (MIR_context_t ctx, FILE *f, MIR_op_t *ops, const char *str) {
  fprintf (f, "if (");
  out_op (ctx, f, ops[1]);
  fprintf (f, " %s ", str);
  out_op (ctx, f, ops[2]);
  fprintf (f, ") ");
  out_jmp (ctx, f, ops[0]);
}

static void out_insn (MIR_context_t ctx, FILE *f, MIR_insn_t insn) {
  MIR_op_t *ops = insn->ops;

  if (insn->code != MIR_LABEL) fprintf (f, "  ");
  switch (insn->code) {
  case MIR_MOV:
  case MIR_FMOV:
  case MIR_DMOV: out_op2 (ctx, f, ops, NULL); break;
  case MIR_EXT8: out_op2 (ctx, f, ops, "(int64_t) (int8_t)"); break;
  case MIR_EXT16: out_op2 (ctx, f, ops, "(int64_t) (int16_t)"); break;
  case MIR_EXT32: out_op2 (ctx, f, ops, "(int64_t) (int32_t)"); break;
  case MIR_UEXT8: out_op2 (ctx, f, ops, "(int64_t) (uint8_t)"); break;
  case MIR_UEXT16: out_op2 (ctx, f, ops, "(int64_t) (uint16_t)"); break;
  case MIR_UEXT32: out_op2 (ctx, f, ops, "(int64_t) (uint32_t)"); break;
  case MIR_F2I:
  case MIR_D2I:
  case MIR_LD2I: out_op2 (ctx, f, ops, "(int64_t)"); break;
  case MIR_I2D:
  case MIR_F2D:
  case MIR_LD2D: out_op2 (ctx, f, ops, "(double)"); break;
  case MIR_I2F:
  case MIR_D2F:
  case MIR_LD2F: out_op2 (ctx, f, ops, "(float)"); break;
  case MIR_I2LD:
  case MIR_D2LD:
  case MIR_F2LD: out_op2 (ctx, f, ops, "(long double)"); break;
  case MIR_UI2D: out_op2 (ctx, f, ops, "(double) (uint64_t)"); break;
  case MIR_UI2F: out_op2 (ctx, f, ops, "(float) (uint64_t)"); break;
  case MIR_UI2LD: out_op2 (ctx, f, ops, "(long double) (uint64_t)"); break;
  case MIR_NEG: out_op2 (ctx, f, ops, "- (int64_t)"); break;
  case MIR_NEGS: out_op2 (ctx, f, ops, "- (int32_t)"); break;
  case MIR_FNEG:
  case MIR_DNEG:
  case MIR_LDNEG: out_op2 (ctx, f, ops, "-"); break;
  case MIR_ADD: out_op3 (ctx, f, ops, "+"); break;
  case MIR_SUB: out_op3 (ctx, f, ops, "-"); break;
  case MIR_MUL: out_op3 (ctx, f, ops, "*"); break;
  case MIR_DIV: out_op3 (ctx, f, ops, "/"); break;
  case MIR_MOD: out_op3 (ctx, f, ops, "%"); break;
  case MIR_UDIV: out_uop3 (ctx, f, ops, "/"); break;
  case MIR_UMOD: out_uop3 (ctx, f, ops, "%"); break;
  case MIR_ADDS: out_sop3 (ctx, f, ops, "+"); break;
  case MIR_SUBS: out_sop3 (ctx, f, ops, "-"); break;
  case MIR_MULS: out_sop3 (ctx, f, ops, "*"); break;
  case MIR_DIVS: out_sop3 (ctx, f, ops, "/"); break;
  case MIR_MODS: out_sop3 (ctx, f, ops, "%"); break;
  case MIR_UDIVS: out_usop3 (ctx, f, ops, "/"); break;
  case MIR_UMODS: out_usop3 (ctx, f, ops, "%"); break;
  case MIR_FADD:
  case MIR_DADD:
  case MIR_LDADD: out_fop3 (ctx, f, ops, "+"); break;
  case MIR_FSUB:
  case MIR_DSUB:
  case MIR_LDSUB: out_fop3 (ctx, f, ops, "-"); break;
  case MIR_FMUL:
  case MIR_DMUL:
  case MIR_LDMUL: out_fop3 (ctx, f, ops, "*"); break;
  case MIR_FDIV:
  case MIR_DDIV:
  case MIR_LDDIV: out_fop3 (ctx, f, ops, "/"); break;
  case MIR_AND: out_op3 (ctx, f, ops, "&"); break;
  case MIR_OR: out_op3 (ctx, f, ops, "|"); break;
  case MIR_XOR: out_op3 (ctx, f, ops, "^"); break;
  case MIR_ANDS: out_sop3 (ctx, f, ops, "&"); break;
  case MIR_ORS: out_sop3 (ctx, f, ops, "|"); break;
  case MIR_XORS: out_sop3 (ctx, f, ops, "^"); break;
  case MIR_LSH: out_op3 (ctx, f, ops, "<<"); break;
  case MIR_RSH: out_op3 (ctx, f, ops, ">>"); break;
  case MIR_URSH: out_uop3 (ctx, f, ops, ">>"); break;
  case MIR_LSHS: out_sop3 (ctx, f, ops, "<<"); break;
  case MIR_RSHS: out_sop3 (ctx, f, ops, ">>"); break;
  case MIR_URSHS: out_usop3 (ctx, f, ops, ">>"); break;
  case MIR_EQ: out_op3 (ctx, f, ops, "=="); break;
  case MIR_NE: out_op3 (ctx, f, ops, "!="); break;
  case MIR_LT: out_op3 (ctx, f, ops, "<"); break;
  case MIR_LE: out_op3 (ctx, f, ops, "<="); break;
  case MIR_GT: out_op3 (ctx, f, ops, ">"); break;
  case MIR_GE: out_op3 (ctx, f, ops, ">="); break;
  case MIR_EQS: out_sop3 (ctx, f, ops, "=="); break;
  case MIR_NES: out_sop3 (ctx, f, ops, "!="); break;
  case MIR_LTS: out_sop3 (ctx, f, ops, "<"); break;
  case MIR_LES: out_sop3 (ctx, f, ops, "<="); break;
  case MIR_GTS: out_sop3 (ctx, f, ops, ">"); break;
  case MIR_GES: out_sop3 (ctx, f, ops, ">="); break;
  case MIR_ULT: out_uop3 (ctx, f, ops, "<"); break;
  case MIR_ULE: out_uop3 (ctx, f, ops, "<="); break;
  case MIR_UGT: out_uop3 (ctx, f, ops, ">"); break;
  case MIR_UGE: out_uop3 (ctx, f, ops, ">"); break;
  case MIR_ULTS: out_usop3 (ctx, f, ops, "<"); break;
  case MIR_ULES: out_usop3 (ctx, f, ops, "<="); break;
  case MIR_UGTS: out_usop3 (ctx, f, ops, ">"); break;
  case MIR_UGES: out_usop3 (ctx, f, ops, ">="); break;
  case MIR_FEQ:
  case MIR_DEQ:
  case MIR_LDEQ: out_fop3 (ctx, f, ops, "=="); break;
  case MIR_FNE:
  case MIR_DNE:
  case MIR_LDNE: out_fop3 (ctx, f, ops, "!="); break;
  case MIR_FLT:
  case MIR_DLT:
  case MIR_LDLT: out_fop3 (ctx, f, ops, "<"); break;
  case MIR_FLE:
  case MIR_DLE:
  case MIR_LDLE: out_fop3 (ctx, f, ops, "<="); break;
  case MIR_FGT:
  case MIR_DGT:
  case MIR_LDGT: out_fop3 (ctx, f, ops, ">"); break;
  case MIR_FGE:
  case MIR_DGE:
  case MIR_LDGE: out_fop3 (ctx, f, ops, ">="); break;
  case MIR_JMP: out_jmp (ctx, f, ops[0]); break;
  case MIR_BT:
  case MIR_BF:
  case MIR_BTS:
  case MIR_BFS:
    fprintf (f, "if (");
    if (insn->code == MIR_BF || insn->code == MIR_BFS) fprintf (f, "!");
    fprintf (f, insn->code == MIR_BF || insn->code == MIR_BT ? "(int64_t) " : "(int32_t) ");
    out_op (ctx, f, ops[1]);
    fprintf (f, ") ");
    out_jmp (ctx, f, ops[0]);
    break;
  case MIR_BEQ: out_bcmp (ctx, f, ops, "=="); break;
  case MIR_BNE: out_bcmp (ctx, f, ops, "!="); break;
  case MIR_BLT: out_bcmp (ctx, f, ops, "<"); break;
  case MIR_BLE: out_bcmp (ctx, f, ops, "<="); break;
  case MIR_BGT: out_bcmp (ctx, f, ops, ">"); break;
  case MIR_BGE: out_bcmp (ctx, f, ops, ">="); break;
  case MIR_BEQS: out_bscmp (ctx, f, ops, "=="); break;
  case MIR_BNES: out_bscmp (ctx, f, ops, "!="); break;
  case MIR_BLTS: out_bscmp (ctx, f, ops, "<"); break;
  case MIR_BLES: out_bscmp (ctx, f, ops, "<="); break;
  case MIR_BGTS: out_bscmp (ctx, f, ops, ">"); break;
  case MIR_BGES: out_bscmp (ctx, f, ops, ">="); break;
  case MIR_UBLT: out_bucmp (ctx, f, ops, "<"); break;
  case MIR_UBLE: out_bucmp (ctx, f, ops, "<="); break;
  case MIR_UBGT: out_bucmp (ctx, f, ops, ">"); break;
  case MIR_UBGE: out_bucmp (ctx, f, ops, ">="); break;
  case MIR_UBLTS: out_buscmp (ctx, f, ops, "<"); break;
  case MIR_UBLES: out_buscmp (ctx, f, ops, "<="); break;
  case MIR_UBGTS: out_buscmp (ctx, f, ops, ">"); break;
  case MIR_UBGES: out_buscmp (ctx, f, ops, ">="); break;
  case MIR_FBEQ:
  case MIR_DBEQ:
  case MIR_LDBEQ: out_bfcmp (ctx, f, ops, "=="); break;
  case MIR_FBNE:
  case MIR_DBNE:
  case MIR_LDBNE: out_bfcmp (ctx, f, ops, "!="); break;
  case MIR_FBLT:
  case MIR_DBLT:
  case MIR_LDBLT: out_bfcmp (ctx, f, ops, "<"); break;
  case MIR_FBLE:
  case MIR_DBLE:
  case MIR_LDBLE: out_bfcmp (ctx, f, ops, "<="); break;
  case MIR_FBGT:
  case MIR_DBGT:
  case MIR_LDBGT: out_bfcmp (ctx, f, ops, ">"); break;
  case MIR_FBGE:
  case MIR_DBGE:
  case MIR_LDBGE: out_bfcmp (ctx, f, ops, ">="); break;
  case MIR_ALLOCA:
    out_op (ctx, f, ops[0]);
    fprintf (f, " = alloca (");
    out_op (ctx, f, ops[1]);
    fprintf (f, ");\n");
    break;
  case MIR_CALL:
  case MIR_INLINE: {
    MIR_proto_t proto;
    size_t start = 2;

    mir_assert (insn->nops >= 2 && ops[0].mode == MIR_OP_REF
                && ops[0].u.ref->item_type == MIR_proto_item);
    proto = ops[0].u.ref->u.proto;
    if (proto->nres > 1) {
      (*MIR_get_error_func (ctx)) (MIR_call_op_error,
                                   " can not translate multiple results functions into C");
    } else if (proto->nres == 1) {
      out_op (ctx, f, ops[2]);
      fprintf (f, " = ");
      start = 3;
    }
    fprintf (f, "((%s) ", proto->name);
    out_op (ctx, f, ops[1]);
    fprintf (f, ") (");
    for (size_t i = start; i < insn->nops; i++) {
      if (i != start) fprintf (f, ", ");
      if (ops[i].mode == MIR_OP_STR) fprintf (f, "(uint64_t) ");
      out_op (ctx, f, ops[i]);
    }
    fprintf (f, ");\n");
    break;
  }
  case MIR_RET:
    fprintf (f, "return ");
    if (insn->nops > 1) {
      fprintf (stderr, "return with multiple values is not implemented\n");
      exit (1);
    }
    if (insn->nops != 0) out_op (ctx, f, ops[0]);
    fprintf (f, ";\n");
    break;
  case MIR_LABEL:
    mir_assert (ops[0].mode == MIR_OP_INT);
    fprintf (f, "l%" PRId64 ":\n", ops[0].u.i);
    break;
  case MIR_ADDO:
  case MIR_SUBO:
  case MIR_MULO:
    fprintf (f, "__overflow = __builtin_%s_overflow((int64_t)",
             insn->code == MIR_ADDO   ? "add"
             : insn->code == MIR_SUBO ? "sub"
                                      : "mul");
    out_op (ctx, f, ops[1]);
    fprintf (f, ", (int64_t)");
    out_op (ctx, f, ops[2]);
    fprintf (f, ", (int64_t *)&");
    out_op (ctx, f, ops[0]);
    fprintf (f, ");\n");
    break;
  case MIR_ADDOS:
  case MIR_SUBOS:
  case MIR_MULOS:
    fprintf (f, "__overflow = __builtin_%s_overflow((int32_t)",
             insn->code == MIR_ADDOS   ? "add"
             : insn->code == MIR_SUBOS ? "sub"
                                       : "mul");
    out_op (ctx, f, ops[1]);
    fprintf (f, ", (int32_t)");
    out_op (ctx, f, ops[2]);
    fprintf (f, ", (int32_t *)&");
    out_op (ctx, f, ops[0]);
    fprintf (f, ");\n");
    break;
  case MIR_UMULO:
    fprintf (f, "__overflow = __builtin_mul_overflow((uint64_t)");
    out_op (ctx, f, ops[1]);
    fprintf (f, ", (uint64_t)");
    out_op (ctx, f, ops[2]);
    fprintf (f, ", (uint64_t *)&");
    out_op (ctx, f, ops[0]);
    fprintf (f, ");\n");
    break;
  case MIR_UMULOS:
    fprintf (f, "__overflow = __builtin_mul_overflow((uint32_t)");
    out_op (ctx, f, ops[1]);
    fprintf (f, ", (uint32_t)");
    out_op (ctx, f, ops[2]);
    fprintf (f, ", (uint32_t *)&");
    out_op (ctx, f, ops[0]);
    fprintf (f, ");\n");
    break;
  case MIR_BO:
  case MIR_UBO:
    fprintf (f, "if (__overflow) ");
    out_jmp (ctx, f, ops[0]);
    break;
  case MIR_BNO:
  case MIR_UBNO:
    fprintf (f, "if (!__overflow) ");
    out_jmp (ctx, f, ops[0]);
    break;
  case MIR_ADDR:
  case MIR_ADDR8:
  case MIR_ADDR16:
  case MIR_ADDR32:
    out_op (ctx, f, ops[0]);
    fprintf (f, " = (int64_t)(");
    fprintf (f, "(char *)&(");
    out_op (ctx, f, ops[1]);
    fprintf (f, ")");
    fprintf (f, "+ (LITLE_ENDIAN ? 0 : %d",
             insn->code == MIR_ADDR8    ? 7
             : insn->code == MIR_ADDR16 ? 6
             : insn->code == MIR_ADDR32 ? 4
                                        : 0);
    fprintf (f, "));\n");
    break;
  case MIR_LADDR:
    out_op (ctx, f, ops[0]);
    fprintf (f, " = (int64_t)&&");
    out_op (ctx, f, ops[1]);
    fprintf (f, ";\n");
    break;
  case MIR_JMPI:
    fprintf (f, "goto *(void *)");
    out_op (ctx, f, ops[0]);
    fprintf (f, ";\n");
    break;
  case MIR_JCALL:
    fprintf (f, "__builtin_jcall(");
    out_op (ctx, f, ops[1]);
    for (size_t i = 2; i < insn->nops; i++) {
      fprintf (f, ", ");
      if (ops[i].mode == MIR_OP_STR) fprintf (f, "(uint64_t) ");
      out_op (ctx, f, ops[i]);
    }
    fprintf (f, ");\n");
    break;
  case MIR_JRET:
    fprintf (f, "__builtin_jret((void *) ");
    out_op (ctx, f, ops[0]);
    fprintf (f, ");\n");
    break;
    /* Assuming the correct (nested) use of the following insns: */
  case MIR_BSTART: fprintf (f, "{ /* block start */\n"); break;
  case MIR_BEND: fprintf (f, "} /* block end */\n"); break;
  case MIR_PRSET:  // nothing
    break;
  case MIR_PRBEQ:  // assuming unknown property (0)
    if ((ops[2].mode == MIR_OP_INT || ops[2].mode == MIR_OP_UINT) && insn->ops[2].u.i == 0) {
      out_jmp (ctx, f, ops[0]);
    }
    break;
  case MIR_PRBNE:  // assuming unknown property (0)
    if ((ops[2].mode == MIR_OP_INT || ops[2].mode == MIR_OP_UINT) && insn->ops[2].u.i != 0) {
      out_jmp (ctx, f, ops[0]);
    }
    break;
  case MIR_VA_ARG: /* result is arg address, operands: va_list addr and memory */
    mir_assert (ops[2].mode == MIR_OP_MEM);
    MIR_type_t t = ops[2].u.mem.type;
    out_type (f, t);
    curr_temp++;
    fprintf (f, " __t%d = va_arg(*(va_list *) ", curr_temp);
    out_op (ctx, f, ops[1]);
    fprintf (f, ", ");
    if (t == MIR_T_I8 || t == MIR_T_U8 || t == MIR_T_I16 || t == MIR_T_U16) {
      fprintf (f, "int");
    } else {
      out_type (f, t);
    }
    fprintf (f, "); ");
    out_op (ctx, f, ops[0]);
    fprintf (f, " = (int64_t) &__t%d;\n", curr_temp);
    break;
  case MIR_VA_BLOCK_ARG: /* result is arg address, operands: va_list addr and integer (size) */
    mir_assert (ops[2].mode == MIR_OP_INT || ops[2].mode == MIR_OP_UINT);
    mir_assert (ops[3].mode == MIR_OP_INT || ops[3].mode == MIR_OP_UINT);
    int64_t s = ops[2].u.i;
    int64_t bt = ops[3].u.i;
#if defined(__riscv) || defined(__aarch64__) && defined(__APPLE__)
    if (s > 16) {
      out_op (ctx, f, ops[0]);
      fprintf (f, " = (int64_t) va_arg(*(va_list *) ");
      out_op (ctx, f, ops[1]);
      fprintf (f, ", void*);\n");
      break;
    }
#endif
    curr_temp++;
    fprintf (f, "struct __s%d {", curr_temp);
    switch (bt) {
#if defined(__x86_64__) || defined(_M_AMD64)
    case 1:
      fprintf (f, "int64_t a1;");
      if (s > 8) fprintf (f, "int64_t a2;");
      break;
    case 2:
      fprintf (f, "double a1;");
      if (s > 8) fprintf (f, "double a2;");
      break;
    case 3: fprintf (f, "int64_t a1; double a2;"); break;
    case 4: fprintf (f, "double a1; int64_t a2;"); break;
#endif
    default:
#if defined(__riscv)
      if (s > 8 && bt == 1) {
        fprintf (f, "long double a[%ld];", (long) ((s + 15) / 16));
        break;
      }
#endif
      fprintf (f, "int64_t a[%ld];", (long) ((s + 7) / 8));
      break;
    }
    fprintf (f, "} __t%d = va_arg(*(va_list *) ", curr_temp);
    out_op (ctx, f, ops[1]);
    fprintf (f, ", struct __s%d); ", curr_temp);
    out_op (ctx, f, ops[0]);
    fprintf (f, " = (int64_t) &__t%d;\n", curr_temp);
    break;
  case MIR_VA_START:
    fprintf (f, "va_start(*(va_list *)");
    out_op (ctx, f, ops[0]);
    if (curr_func->nargs == 0) {
      fprintf (stderr, "cannot translate va_start in func %s w/o any arg\n", curr_func->name);
      exit (1);
    }
    fprintf (f, ", %s);\n", VARR_GET (MIR_var_t, curr_func->vars, curr_func->nargs - 1).name);
    break;
  case MIR_VA_END:
    fprintf (f, "va_end(*(va_list *)");
    out_op (ctx, f, ops[0]);
    fprintf (f, ");\n");
    break;
    /* operand is va_list */
  default: mir_assert (FALSE);
  }
}

static void out_func_decl (MIR_context_t ctx, FILE *f, MIR_func_t func) {
  MIR_var_t var;
  size_t i;
  if (func->nres == 0)
    fprintf (f, "void");
  else if (func->nres == 1)
    out_type (f, func->res_types[0]);
  else
    (*MIR_get_error_func (ctx)) (MIR_func_error,
                                 "Multiple result functions can not be represented in C");
  fprintf (f, " %s (", func->name);
  for (i = 0; i < func->nargs; i++) {
    if (i != 0) fprintf (f, ", ");
    var = VARR_GET (MIR_var_t, func->vars, i);
    out_type (f, var.type);
    fprintf (f,
             var.type == MIR_T_I64 || var.type == MIR_T_F || var.type == MIR_T_D
                 || var.type == MIR_T_LD
               ? " %s"
               : " _%s",
             var.name);
  }
  if (func->vararg_p) {
    if (i != 0) fprintf (f, ", ");
    fprintf (f, "...");
  } else if (i == 0) {
    fprintf (f, "void");
  }
  fprintf (f, ")");
}

static void out_item (MIR_context_t ctx, FILE *f, MIR_item_t item) {
  MIR_var_t var;
  size_t i, nlocals;

  if (item->item_type == MIR_export_item || item->addr != NULL) return;
  if (item->item_type == MIR_import_item) {
    fprintf (f, "extern char %s[];\n", item->u.import_id);
    return;
  }
  if (item->item_type == MIR_forward_item) {  // ???
    if (item->ref_def == NULL) return;
    if (item->ref_def->item_type == MIR_func_item) {
      out_func_decl (ctx, f, item->ref_def->u.func);
      fprintf (f, ";\n");
    } else {
      out_item (ctx, f, item->ref_def);
      item->ref_def->addr = (char *) 1; /* mark as processed */
    }
    return;
  }
  if (item->item_type == MIR_proto_item) {
    MIR_proto_t proto = item->u.proto;

    fprintf (f, "typedef ");
    if (proto->nres == 0)
      fprintf (f, "void");
    else if (proto->nres == 1)
      out_type (f, proto->res_types[0]);
    else
      (*MIR_get_error_func (ctx)) (MIR_func_error,
                                   "Multiple result functions can not be called in C");
    fprintf (f, " (*%s) (", proto->name);
    for (i = 0; i < VARR_LENGTH (MIR_var_t, proto->args); i++) {
      var = VARR_GET (MIR_var_t, proto->args, i);
      if (i != 0) fprintf (f, ", ");
      out_type (f, var.type);
      if (var.name != NULL) fprintf (f, " %s", var.name);
    }
    if (proto->vararg_p) {
      if (i != 0) fprintf (f, ", ");
      fprintf (f, "...");
    } else if (i == 0) {
      fprintf (f, "void");
    }
    fprintf (f, ");\n");
    return;
  }
  if (!item->export_p) fprintf (f, "static ");
  switch (item->item_type) {
  case MIR_bss_item:
  case MIR_data_item:
  case MIR_ref_data_item:
  case MIR_expr_data_item: {
    MIR_item_t curr_item, next_item;
    const char *name, *curr_name;
    int data_p, struct_p, stop_p, iter, n;
    if ((name = MIR_item_name (ctx, item)) == NULL) return; /* skip part of a section */
    data_p = struct_p = FALSE;
    if ((next_item = DLIST_NEXT (MIR_item_t, item)) != NULL
        && MIR_item_name (ctx, next_item) == NULL) {
      struct_p = TRUE;
      fprintf (f, "struct {");
    }
    for (iter = 0; iter < 2; iter++) {
      for (n = 0, curr_item = item; curr_item != NULL;
           curr_item = DLIST_NEXT (MIR_item_t, item), n++) {
        if ((curr_name = MIR_item_name (ctx, curr_item)) != NULL && curr_item != item)
          break; /* the next section */
        stop_p = FALSE;
        switch (curr_item->item_type) {
        case MIR_data_item:
          if (iter == 0) {
            out_type (f, curr_item->u.data->el_type);
            if (curr_name != NULL)
              fprintf (f, " %s", curr_name);
            else
              fprintf (f, " _m%d", n);
            if (curr_item->u.data->nel != 1)
              fprintf (f, "[%lu]", (unsigned long) curr_item->u.data->nel);
            if (struct_p) fprintf (f, ";");
          } else {
            if (curr_item->u.data->nel != 1) fprintf (f, "{");
            _MIR_output_data_item_els (ctx, f, curr_item, TRUE);
            if (curr_item->u.data->nel != 1) fprintf (f, "}");
          }
          data_p = TRUE;
          break;
        case MIR_ref_data_item:
          data_p = TRUE;
          if (iter == 0) {
            if (curr_name != NULL)
              fprintf (f, "const char *%s", curr_name);
            else
              fprintf (f, "const char *_m%d", n);
            if (struct_p) fprintf (f, ";");
          } else {
            fprintf (f, "(const char *) &%s + %" PRId64,
                     MIR_item_name (ctx, curr_item->u.ref_data->ref_item),
                     (int64_t) curr_item->u.ref_data->disp);
          }
          break;
        case MIR_expr_data_item:
          data_p = TRUE;
          (*MIR_get_error_func (ctx)) (MIR_call_op_error,
                                       " can not translate MIR expr data func into C");
          break;
        case MIR_bss_item:
          if (iter == 0) {
            if (curr_name != NULL)
              fprintf (f, "char %s", curr_name);
            else
              fprintf (f, "char _m%d", n);
            if (curr_item->u.bss->len != 0)
              fprintf (f, "[%lu]", (unsigned long) curr_item->u.bss->len);
          } else if (data_p) {
            mir_assert (struct_p);
            if (curr_item->u.data->nel != 1) fprintf (f, "{");
            for (size_t nel = 0; nel < curr_item->u.data->nel; nel++) fprintf (f, "0, ");
            if (curr_item->u.data->nel != 1) fprintf (f, "}");
          }
          break;
        default: stop_p = TRUE; break;
        }
        if (stop_p) break;
      }
      if (iter == 0) {
        if (struct_p) fprintf (f, "} %s", name);
        if (data_p) {
          fprintf (f, " = ");
          if (struct_p) fprintf (f, " {");
        }
      } else if (data_p && struct_p) {
        fprintf (f, ", ");
      }
      if (stop_p) break;
    }
    fprintf (f, ";\n");
    return;
  }
  default: mir_assert (item->item_type == MIR_func_item);
  }
  curr_func = item->u.func;
  out_func_decl (ctx, f, curr_func);
  fprintf (f, " {\n");
  curr_temp = 0;
  for (i = 0; i < curr_func->nargs; i++) {
    var = VARR_GET (MIR_var_t, curr_func->vars, i);
    if (var.type == MIR_T_I64 || var.type == MIR_T_F || var.type == MIR_T_D || var.type == MIR_T_LD)
      continue;
    fprintf (f, "  int64_t %s = _%s;\n", var.name, var.name);
  }
  nlocals = VARR_LENGTH (MIR_var_t, curr_func->vars) - curr_func->nargs;
  for (i = 0; i < nlocals; i++) {
    var = VARR_GET (MIR_var_t, curr_func->vars, i + curr_func->nargs);
    fprintf (f, "  ");
    out_type (f, var.type);
    fprintf (f, " %s;\n", var.name);
  }
  fprintf (f, "  int __overflow;\n");
  fprintf (f, "  const int LITLE_ENDIAN_X = 1;\n");
  fprintf (f, "  const int LITLE_ENDIAN = *(char *) &LITLE_ENDIAN_X;\n");
  for (MIR_insn_t insn = DLIST_HEAD (MIR_insn_t, curr_func->insns); insn != NULL;
       insn = DLIST_NEXT (MIR_insn_t, insn))
    out_insn (ctx, f, insn);
  fprintf (f, "}\n");
}

void MIR_module2c (MIR_context_t ctx, FILE *f, MIR_module_t m) {
  fprintf (f, "#include <stdint.h>\n#include <stdarg.h>\n");
  for (MIR_item_t item = DLIST_HEAD (MIR_item_t, m->items); item != NULL;
       item = DLIST_NEXT (MIR_item_t, item))
    out_item (ctx, f, item);
}

/* ------------------------- Small test example ------------------------- */
#if defined(TEST_MIR2C)

#include "mir-tests/scan-sieve.h"
#include "mir-tests/scan-hi.h"

MIR_module_t create_ext_module (MIR_context_t ctx) {
  const char *str
    = "\n\
m_ext:   module\n\
p:	 proto i64:a, ...\n\
ext:     func i64:a, ...\n\
         local i64:i,i64:j,i64:k,i64:va\n\
         va_start va\n\
         va_arg i,va,i8:0\n\
         va_block_arg i, va, 40, 0\n\
         va_block_arg i, va, 16, 1\n\
         va_block_arg i, va, 16, 2\n\
         va_block_arg i, va, 16, 3\n\
         va_block_arg i, va, 16, 4\n\
         va_end va\n\
l5:\n\
         bstart i\n\
         bend i\n\
l6:\n\
         addo i,j,k\n\
         addos i,j,k\n\
         subo i,j,k\n\
         subos i,j,k\n\
         mulo i,j,k\n\
         bo l5\n\
         mulos i,j,k\n\
         bno l6\n\
         umulo i,j,k\n\
         ubo l5\n\
         umulos i,j,k\n\
         ubno l6\n\
         addr i,i\n\
         addr8 i,i\n\
         addr16 i,i\n\
         addr32 i,i\n\
         laddr i,l5\n\
         jmpi i\n\
         jret i\n\
         endfunc\n\
ext2:    func\n\
         jcall p,ext,10\n\
         endfunc\n\
         endmodule\n\
";

  MIR_scan_string (ctx, str);
  return DLIST_TAIL (MIR_module_t, *MIR_get_module_list (ctx));
}

int main (int argc, const char *argv[]) {
  MIR_module_t m;
  MIR_context_t ctx = MIR_init ();

  create_mir_func_sieve (ctx, NULL, &m);
  MIR_module2c (ctx, stdout, m);
  m = create_hi_module (ctx);
  MIR_module2c (ctx, stdout, m);
  m = create_ext_module (ctx);
  MIR_module2c (ctx, stdout, m);
  MIR_finish (ctx);
  return 0;
}
#elif defined(MIR2C)

DEF_VARR (char);

int main (int argc, const char *argv[]) {
  int c;
  FILE *f;
  VARR (char) * input;
  MIR_module_t m;
  MIR_context_t ctx = MIR_init ();
  MIR_alloc_t alloc = MIR_get_alloc (ctx);

  if (argc == 1)
    f = stdin;
  else if (argc == 2) {
    if ((f = fopen (argv[1], "r")) == NULL) {
      fprintf (stderr, "%s: cannot open file %s\n", argv[0], argv[1]);
      exit (1);
    }
  } else {
    fprintf (stderr, "usage: %s < file or %s mir-file\n", argv[0], argv[0]);
    exit (1);
  }
  VARR_CREATE (char, input, alloc, 0);
  while ((c = getc (f)) != EOF) VARR_PUSH (char, input, c);
  VARR_PUSH (char, input, 0);
  if (ferror (f)) {
    fprintf (stderr, "%s: error in reading input file\n", argv[0]);
    exit (1);
  }
  fclose (f);
  MIR_scan_string (ctx, VARR_ADDR (char, input));
  m = DLIST_TAIL (MIR_module_t, *MIR_get_module_list (ctx));
  MIR_module2c (ctx, stdout, m);
  MIR_finish (ctx);
  VARR_DESTROY (char, input);
  return 0;
}
#endif
