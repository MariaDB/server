/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include "mir.h"
#include "mir-code-alloc.h"

DEF_VARR (MIR_insn_t);
DEF_VARR (MIR_reg_t);
DEF_VARR (MIR_op_t);
DEF_VARR (MIR_type_t);
DEF_HTAB (MIR_item_t);
DEF_VARR (MIR_module_t);
DEF_VARR (size_t);
DEF_VARR (char);
DEF_VARR (uint8_t);
DEF_VARR (MIR_proto_t);

struct gen_ctx;
struct c2mir_ctx;
struct string_ctx;
struct reg_ctx;
struct alias_ctx;
struct simplify_ctx;
struct machine_code_ctx;
struct io_ctx;
struct scan_ctx;
struct hard_reg_ctx;
struct interp_ctx;

struct MIR_context {
  struct gen_ctx *gen_ctx;     /* should be the 1st member */
  struct c2mir_ctx *c2mir_ctx; /* should be the 2nd member */
  MIR_error_func_t error_func;
  MIR_alloc_t alloc;
  MIR_code_alloc_t code_alloc;
  int func_redef_permission_p;        /* when true loaded func can be redfined lately */
  VARR (size_t) * insn_nops;          /* constant after initialization */
  VARR (MIR_proto_t) * unspec_protos; /* protos of unspec insns (set only during initialization) */
  VARR (char) * temp_string;
  VARR (uint8_t) * temp_data, *used_label_p;
  HTAB (MIR_item_t) * module_item_tab;
  /* Module to keep items potentially used by all modules:  */
  struct MIR_module environment_module;
  MIR_module_t curr_module;
  MIR_func_t curr_func;
  size_t curr_label_num;
  DLIST (MIR_module_t) all_modules;
  VARR (MIR_module_t) * modules_to_link;
  VARR (MIR_op_t) * temp_ops;
  struct string_ctx *string_ctx;
  struct reg_ctx *reg_ctx;
  struct alias_ctx *alias_ctx;
  struct simplify_ctx *simplify_ctx;
  struct machine_code_ctx *machine_code_ctx;
  struct io_ctx *io_ctx;
  struct scan_ctx *scan_ctx;
  struct hard_reg_ctx *hard_reg_ctx;
  struct interp_ctx *interp_ctx;
  void *setjmp_addr;      /* used in interpreter to call setjmp directly not from a shim and FFI */
  void *wrapper_end_addr; /* used by generator */
};

#define error_func ctx->error_func
#define func_redef_permission_p ctx->func_redef_permission_p
#define unspec_protos ctx->unspec_protos
#define insn_nops ctx->insn_nops
#define temp_string ctx->temp_string
#define temp_data ctx->temp_data
#define used_label_p ctx->used_label_p
#define module_item_tab ctx->module_item_tab
#define environment_module ctx->environment_module
#define curr_module ctx->curr_module
#define curr_func ctx->curr_func
#define curr_label_num ctx->curr_label_num
#define all_modules ctx->all_modules
#define modules_to_link ctx->modules_to_link
#define temp_ops ctx->temp_ops
#define setjmp_addr ctx->setjmp_addr
#define wrapper_end_addr ctx->wrapper_end_addr

static void util_error (MIR_context_t ctx, const char *message);
#define MIR_VARR_ERROR util_error
#define MIR_HTAB_ERROR MIR_VARR_ERROR

#include "mir-hash.h"
#include "mir-htab.h"
#include "mir-reduce.h"
#include "mir-bitmap.h"
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <float.h>
#include <ctype.h>
#include <limits.h>

static void interp_init (MIR_context_t ctx);
static void finish_func_interpretation (MIR_item_t func_item, MIR_alloc_t alloc);
static void interp_finish (MIR_context_t ctx);

static void MIR_NO_RETURN default_error (enum MIR_error_type error_type MIR_UNUSED,
                                         const char *format, ...) {
  va_list ap;

  va_start (ap, format);
  vfprintf (stderr, format, ap);
  fprintf (stderr, "\n");
  va_end (ap);
  exit (1);
}

static void MIR_NO_RETURN MIR_UNUSED util_error (MIR_context_t ctx, const char *message) {
  MIR_get_error_func (ctx) (MIR_alloc_error, message);
}

#define HARD_REG_NAME_PREFIX "hr"
#define TEMP_REG_NAME_PREFIX "t"
#define TEMP_ITEM_NAME_PREFIX ".lc"

int _MIR_reserved_ref_name_p (MIR_context_t ctx MIR_UNUSED, const char *name) {
  return strncmp (name, TEMP_ITEM_NAME_PREFIX, strlen (TEMP_ITEM_NAME_PREFIX)) == 0;
}

/* Reserved names:
   fp - frame pointer
   hr<number> - a hardware reg
   lc<number> - a temp item */
int _MIR_reserved_name_p (MIR_context_t ctx, const char *name) {
  size_t i, start;

  if (_MIR_reserved_ref_name_p (ctx, name))
    return TRUE;
  else if (strncmp (name, HARD_REG_NAME_PREFIX, strlen (HARD_REG_NAME_PREFIX)) == 0)
    start = strlen (HARD_REG_NAME_PREFIX);
  else
    return FALSE;
  for (i = start; name[i] != '\0'; i++)
    if (name[i] < '0' || name[i] > '9') return FALSE;
  return TRUE;
}

struct insn_desc {
  MIR_insn_code_t code;
  const char *name;
  unsigned char op_modes[5];
};

#define OUT_FLAG (1 << 7)

static const struct insn_desc insn_descs[] = {
  {MIR_MOV, "mov", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FMOV, "fmov", {MIR_OP_FLOAT | OUT_FLAG, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DMOV, "dmov", {MIR_OP_DOUBLE | OUT_FLAG, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDMOV, "ldmov", {MIR_OP_LDOUBLE | OUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_EXT8, "ext8", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_EXT16, "ext16", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_EXT32, "ext32", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UEXT8, "uext8", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UEXT16, "uext16", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UEXT32, "uext32", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_I2F, "i2f", {MIR_OP_FLOAT | OUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_I2D, "i2d", {MIR_OP_DOUBLE | OUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_I2LD, "i2ld", {MIR_OP_LDOUBLE | OUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UI2F, "ui2f", {MIR_OP_FLOAT | OUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UI2D, "ui2d", {MIR_OP_DOUBLE | OUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UI2LD, "ui2ld", {MIR_OP_LDOUBLE | OUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_F2I, "f2i", {MIR_OP_INT | OUT_FLAG, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_D2I, "d2i", {MIR_OP_INT | OUT_FLAG, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LD2I, "ld2i", {MIR_OP_INT | OUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_F2D, "f2d", {MIR_OP_DOUBLE | OUT_FLAG, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_F2LD, "f2ld", {MIR_OP_LDOUBLE | OUT_FLAG, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_D2F, "d2f", {MIR_OP_FLOAT | OUT_FLAG, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_D2LD, "d2ld", {MIR_OP_LDOUBLE | OUT_FLAG, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LD2F, "ld2f", {MIR_OP_FLOAT | OUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_LD2D, "ld2d", {MIR_OP_DOUBLE | OUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_NEG, "neg", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_NEGS, "negs", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FNEG, "fneg", {MIR_OP_FLOAT | OUT_FLAG, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DNEG, "dneg", {MIR_OP_DOUBLE | OUT_FLAG, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDNEG, "ldneg", {MIR_OP_LDOUBLE | OUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_ADDR, "addr", {MIR_OP_INT | OUT_FLAG, MIR_OP_REG, MIR_OP_BOUND}},     /* MIR_OP_REG! */
  {MIR_ADDR8, "addr8", {MIR_OP_INT | OUT_FLAG, MIR_OP_REG, MIR_OP_BOUND}},   /* MIR_OP_REG! */
  {MIR_ADDR16, "addr16", {MIR_OP_INT | OUT_FLAG, MIR_OP_REG, MIR_OP_BOUND}}, /* MIR_OP_REG! */
  {MIR_ADDR32, "addr32", {MIR_OP_INT | OUT_FLAG, MIR_OP_REG, MIR_OP_BOUND}}, /* MIR_OP_REG! */
  {MIR_ADD, "add", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_ADDS, "adds", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FADD, "fadd", {MIR_OP_FLOAT | OUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DADD, "dadd", {MIR_OP_DOUBLE | OUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDADD, "ldadd", {MIR_OP_LDOUBLE | OUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_SUB, "sub", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_SUBS, "subs", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FSUB, "fsub", {MIR_OP_FLOAT | OUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DSUB, "dsub", {MIR_OP_DOUBLE | OUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDSUB, "ldsub", {MIR_OP_LDOUBLE | OUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_MUL, "mul", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_MULS, "muls", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FMUL, "fmul", {MIR_OP_FLOAT | OUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DMUL, "dmul", {MIR_OP_DOUBLE | OUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDMUL, "ldmul", {MIR_OP_LDOUBLE | OUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_DIV, "div", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_DIVS, "divs", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UDIV, "udiv", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UDIVS, "udivs", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FDIV, "fdiv", {MIR_OP_FLOAT | OUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DDIV, "ddiv", {MIR_OP_DOUBLE | OUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDDIV, "lddiv", {MIR_OP_LDOUBLE | OUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_MOD, "mod", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_MODS, "mods", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UMOD, "umod", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UMODS, "umods", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_AND, "and", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_ANDS, "ands", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_OR, "or", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_ORS, "ors", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_XOR, "xor", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_XORS, "xors", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_LSH, "lsh", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_LSHS, "lshs", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_RSH, "rsh", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_RSHS, "rshs", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_URSH, "ursh", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_URSHS, "urshs", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_EQ, "eq", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_EQS, "eqs", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FEQ, "feq", {MIR_OP_INT | OUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DEQ, "deq", {MIR_OP_INT | OUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDEQ, "ldeq", {MIR_OP_INT | OUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_NE, "ne", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_NES, "nes", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FNE, "fne", {MIR_OP_INT | OUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DNE, "dne", {MIR_OP_INT | OUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDNE, "ldne", {MIR_OP_INT | OUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_LT, "lt", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_LTS, "lts", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_ULT, "ult", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_ULTS, "ults", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FLT, "flt", {MIR_OP_INT | OUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DLT, "dlt", {MIR_OP_INT | OUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDLT, "ldlt", {MIR_OP_INT | OUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_LE, "le", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_LES, "les", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_ULE, "ule", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_ULES, "ules", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FLE, "fle", {MIR_OP_INT | OUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DLE, "dle", {MIR_OP_INT | OUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDLE, "ldle", {MIR_OP_INT | OUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_GT, "gt", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_GTS, "gts", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UGT, "ugt", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UGTS, "ugts", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FGT, "fgt", {MIR_OP_INT | OUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DGT, "dgt", {MIR_OP_INT | OUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDGT, "ldgt", {MIR_OP_INT | OUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_GE, "ge", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_GES, "ges", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UGE, "uge", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UGES, "uges", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FGE, "fge", {MIR_OP_INT | OUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DGE, "dge", {MIR_OP_INT | OUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDGE, "ldge", {MIR_OP_INT | OUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_ADDO, "addo", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_ADDOS, "addos", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_SUBO, "subo", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_SUBOS, "subos", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_MULO, "mulo", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_MULOS, "mulos", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UMULO, "umulo", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UMULOS, "umulos", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_JMP, "jmp", {MIR_OP_LABEL, MIR_OP_BOUND}},
  {MIR_BT, "bt", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BTS, "bts", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BF, "bf", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BFS, "bfs", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BEQ, "beq", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BEQS, "beqs", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FBEQ, "fbeq", {MIR_OP_LABEL, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DBEQ, "dbeq", {MIR_OP_LABEL, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDBEQ, "ldbeq", {MIR_OP_LABEL, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_BNE, "bne", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BNES, "bnes", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FBNE, "fbne", {MIR_OP_LABEL, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DBNE, "dbne", {MIR_OP_LABEL, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDBNE, "ldbne", {MIR_OP_LABEL, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_BLT, "blt", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BLTS, "blts", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UBLT, "ublt", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UBLTS, "ublts", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FBLT, "fblt", {MIR_OP_LABEL, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DBLT, "dblt", {MIR_OP_LABEL, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDBLT, "ldblt", {MIR_OP_LABEL, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_BLE, "ble", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BLES, "bles", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UBLE, "uble", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UBLES, "ubles", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FBLE, "fble", {MIR_OP_LABEL, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DBLE, "dble", {MIR_OP_LABEL, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDBLE, "ldble", {MIR_OP_LABEL, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_BGT, "bgt", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BGTS, "bgts", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UBGT, "ubgt", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UBGTS, "ubgts", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FBGT, "fbgt", {MIR_OP_LABEL, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DBGT, "dbgt", {MIR_OP_LABEL, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDBGT, "ldbgt", {MIR_OP_LABEL, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_BGE, "bge", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BGES, "bges", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UBGE, "ubge", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UBGES, "ubges", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FBGE, "fbge", {MIR_OP_LABEL, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DBGE, "dbge", {MIR_OP_LABEL, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDBGE, "ldbge", {MIR_OP_LABEL, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_BO, "bo", {MIR_OP_LABEL, MIR_OP_BOUND}},
  {MIR_UBO, "ubo", {MIR_OP_LABEL, MIR_OP_BOUND}},
  {MIR_BNO, "bno", {MIR_OP_LABEL, MIR_OP_BOUND}},
  {MIR_UBNO, "ubno", {MIR_OP_LABEL, MIR_OP_BOUND}},
  {MIR_LADDR, "laddr", {MIR_OP_INT, MIR_OP_LABEL, MIR_OP_BOUND}},
  {MIR_JMPI, "jmpi", {MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_CALL, "call", {MIR_OP_BOUND}},
  {MIR_INLINE, "inline", {MIR_OP_BOUND}},
  {MIR_JCALL, "jcall", {MIR_OP_BOUND}},
  {MIR_SWITCH, "switch", {MIR_OP_BOUND}},
  {MIR_RET, "ret", {MIR_OP_BOUND}},
  {MIR_JRET, "jret", {MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_ALLOCA, "alloca", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BSTART, "bstart", {MIR_OP_INT | OUT_FLAG, MIR_OP_BOUND}},
  {MIR_BEND, "bend", {MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_VA_ARG, "va_arg", {MIR_OP_INT | OUT_FLAG, MIR_OP_INT, MIR_OP_UNDEF, MIR_OP_BOUND}},
  {MIR_VA_BLOCK_ARG,
   "va_block_arg",
   {MIR_OP_INT, MIR_OP_INT, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_VA_START, "va_start", {MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_VA_END, "va_end", {MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_LABEL, "label", {MIR_OP_BOUND}},
  {MIR_UNSPEC, "unspec", {MIR_OP_BOUND}},
  {MIR_PRSET, "prset", {MIR_OP_UNDEF, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_PRBEQ, "prbeq", {MIR_OP_LABEL, MIR_OP_UNDEF, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_PRBNE, "prbne", {MIR_OP_LABEL, MIR_OP_UNDEF, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_USE, "use", {MIR_OP_BOUND}},
  {MIR_PHI, "phi", {MIR_OP_BOUND}},
  {MIR_INVALID_INSN, "invalid-insn", {MIR_OP_BOUND}},
};

static void check_and_prepare_insn_descs (MIR_context_t ctx) {
  size_t i, j;

  VARR_CREATE (size_t, insn_nops, ctx->alloc, 0);
  for (i = 0; i < MIR_INSN_BOUND; i++) {
    mir_assert (insn_descs[i].code == i);
    for (j = 0; insn_descs[i].op_modes[j] != MIR_OP_BOUND; j++)
      ;
    VARR_PUSH (size_t, insn_nops, j);
  }
}

static MIR_op_mode_t type2mode (MIR_type_t type) {
  return (type == MIR_T_UNDEF ? MIR_OP_UNDEF
          : type == MIR_T_F   ? MIR_OP_FLOAT
          : type == MIR_T_D   ? MIR_OP_DOUBLE
          : type == MIR_T_LD  ? MIR_OP_LDOUBLE
                              : MIR_OP_INT);
}

int64_t _MIR_addr_offset (MIR_context_t ctx MIR_UNUSED, MIR_insn_code_t code) {
  int v = 1;
  if (code == MIR_ADDR || *(char *) &v != 0) return 0;
  return code == MIR_ADDR8 ? 7 : code == MIR_ADDR16 ? 6 : 4;
}

/* New Page */

typedef struct string {
  size_t num; /* string number starting with 1 */
  MIR_str_t str;
} string_t;

DEF_VARR (string_t);
DEF_HTAB (string_t);

struct string_ctx {
  VARR (string_t) * strings;
  HTAB (string_t) * string_tab;
};

#define strings ctx->string_ctx->strings
#define string_tab ctx->string_ctx->string_tab

static htab_hash_t str_hash (string_t str, void *arg MIR_UNUSED) {
  return (htab_hash_t) mir_hash (str.str.s, str.str.len, 0);
}
static int str_eq (string_t str1, string_t str2, void *arg MIR_UNUSED) {
  return str1.str.len == str2.str.len && memcmp (str1.str.s, str2.str.s, str1.str.len) == 0;
}

static void string_init (MIR_alloc_t alloc, VARR (string_t) * *strs, HTAB (string_t) * *str_tab) {
  string_t string = {0, {0, NULL}};

  VARR_CREATE (string_t, *strs, alloc, 0);
  VARR_PUSH (string_t, *strs, string); /* don't use 0th string */
  HTAB_CREATE (string_t, *str_tab, alloc, 1000, str_hash, str_eq, NULL);
}

static int string_find (VARR (string_t) * *strs MIR_UNUSED, HTAB (string_t) * *str_tab,
                        MIR_str_t str, string_t *s) {
  string_t string;

  string.str = str;
  return HTAB_DO (string_t, *str_tab, string, HTAB_FIND, *s);
}

static string_t string_store (MIR_context_t ctx, VARR (string_t) * *strs,
                              HTAB (string_t) * *str_tab, MIR_str_t str) {
  char *heap_str;
  string_t el, string;

  if (string_find (strs, str_tab, str, &el)) return el;
  if ((heap_str = MIR_malloc (ctx->alloc, str.len)) == NULL)
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for strings");
  memcpy (heap_str, str.s, str.len);
  string.str.s = heap_str;
  string.str.len = str.len;
  string.num = VARR_LENGTH (string_t, *strs);
  VARR_PUSH (string_t, *strs, string);
  HTAB_DO (string_t, *str_tab, string, HTAB_INSERT, el);
  return string;
}

static struct string get_ctx_string (MIR_context_t ctx, MIR_str_t str) {
  return string_store (ctx, &strings, &string_tab, str);
}

static const char *get_ctx_str (MIR_context_t ctx, const char *string) {
  return get_ctx_string (ctx, (MIR_str_t){strlen (string) + 1, string}).str.s;
}

static void string_finish (MIR_alloc_t alloc, VARR (string_t) * *strs, HTAB (string_t) * *str_tab) {
  size_t i;

  for (i = 1; i < VARR_LENGTH (string_t, *strs); i++)
    MIR_free (alloc, (char *) VARR_ADDR (string_t, *strs)[i].str.s);
  VARR_DESTROY (string_t, *strs);
  HTAB_DESTROY (string_t, *str_tab);
}

/* Functions to work with aliases.  */

struct alias_ctx {
  VARR (string_t) * aliases;
  HTAB (string_t) * alias_tab;
};

#define aliases ctx->alias_ctx->aliases
#define alias_tab ctx->alias_ctx->alias_tab

MIR_alias_t MIR_alias (MIR_context_t ctx, const char *name) {
  return (MIR_alias_t) string_store (ctx, &aliases, &alias_tab,
                                     (MIR_str_t){strlen (name) + 1, name})
    .num;
}

const char *MIR_alias_name (MIR_context_t ctx, MIR_alias_t alias) {
  if (alias == 0) return "";
  if (alias >= VARR_LENGTH (string_t, aliases))
    MIR_get_error_func (ctx) (MIR_alloc_error, "Wrong alias number");
  return VARR_ADDR (string_t, aliases)[alias].str.s;
}

/* New Page */

/* We attribute global vars to func as func can be inlined from different module.  */
typedef struct reg_desc {
  MIR_type_t type;
  MIR_reg_t reg;       /* key reg2rdn hash tab */
  char *name;          /* key for the name2rdn hash tab */
  char *hard_reg_name; /* NULL unless tied global, key for hrn2rdn */
} reg_desc_t;

DEF_VARR (reg_desc_t);

DEF_HTAB (size_t);

typedef struct func_regs {
  VARR (reg_desc_t) * reg_descs;
  HTAB (size_t) * name2rdn_tab;
  HTAB (size_t) * hrn2rdn_tab;
  HTAB (size_t) * reg2rdn_tab;
} *func_regs_t;

static int name2rdn_eq (size_t rdn1, size_t rdn2, void *arg) {
  func_regs_t func_regs = arg;
  reg_desc_t *addr = VARR_ADDR (reg_desc_t, func_regs->reg_descs);

  return strcmp (addr[rdn1].name, addr[rdn2].name) == 0;
}

static htab_hash_t name2rdn_hash (size_t rdn, void *arg) {
  func_regs_t func_regs = arg;
  reg_desc_t *addr = VARR_ADDR (reg_desc_t, func_regs->reg_descs);

  return (htab_hash_t) mir_hash (addr[rdn].name, strlen (addr[rdn].name), 0);
}

static int hrn2rdn_eq (size_t rdn1, size_t rdn2, void *arg) {
  func_regs_t func_regs = arg;
  reg_desc_t *addr = VARR_ADDR (reg_desc_t, func_regs->reg_descs);

  return strcmp (addr[rdn1].hard_reg_name, addr[rdn2].hard_reg_name) == 0;
}

static htab_hash_t hrn2rdn_hash (size_t rdn, void *arg) {
  func_regs_t func_regs = arg;
  reg_desc_t *addr = VARR_ADDR (reg_desc_t, func_regs->reg_descs);

  return (htab_hash_t) mir_hash (addr[rdn].hard_reg_name, strlen (addr[rdn].hard_reg_name), 0);
}

static int reg2rdn_eq (size_t rdn1, size_t rdn2, void *arg) {
  func_regs_t func_regs = arg;
  reg_desc_t *addr = VARR_ADDR (reg_desc_t, func_regs->reg_descs);

  return addr[rdn1].reg == addr[rdn2].reg;
}

static htab_hash_t reg2rdn_hash (size_t rdn, void *arg) {
  func_regs_t func_regs = arg;
  reg_desc_t *addr = VARR_ADDR (reg_desc_t, func_regs->reg_descs);

  return (htab_hash_t) mir_hash_finish (mir_hash_step (mir_hash_init (0), addr[rdn].reg));
}

static void func_regs_init (MIR_context_t ctx, MIR_func_t func) {
  func_regs_t func_regs;
  reg_desc_t rd = {MIR_T_I64, 0, NULL, NULL};

  if ((func_regs = func->internal = MIR_malloc (ctx->alloc, sizeof (struct func_regs))) == NULL)
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for func regs info");
  VARR_CREATE (reg_desc_t, func_regs->reg_descs, ctx->alloc, 50);
  VARR_PUSH (reg_desc_t, func_regs->reg_descs, rd); /* for 0 reg */
  HTAB_CREATE (size_t, func_regs->name2rdn_tab, ctx->alloc, 100, name2rdn_hash, name2rdn_eq, func_regs);
  HTAB_CREATE (size_t, func_regs->hrn2rdn_tab, ctx->alloc, 10, hrn2rdn_hash, hrn2rdn_eq, func_regs);
  HTAB_CREATE (size_t, func_regs->reg2rdn_tab, ctx->alloc, 100, reg2rdn_hash, reg2rdn_eq, func_regs);
}

static int target_locs_num (MIR_reg_t loc, MIR_type_t type);
static int target_hard_reg_type_ok_p (MIR_reg_t hard_reg, MIR_type_t type);
static int target_fixed_hard_reg_p (MIR_reg_t hard_reg);

static MIR_reg_t create_func_reg (MIR_context_t ctx, MIR_func_t func, const char *name,
                                  const char *hard_reg_name, MIR_reg_t reg, MIR_type_t type,
                                  int any_p, char **name_ptr) {
  func_regs_t func_regs = func->internal;
  MIR_module_t func_module;
  reg_desc_t rd, *rd_ref;
  size_t rdn, tab_rdn;
  int htab_res;
  MIR_reg_t hr;

  if (!any_p && _MIR_reserved_name_p (ctx, name))
    MIR_get_error_func (ctx) (MIR_reserved_name_error, "redefining a reserved name %s", name);
  rd.name = (char *) get_ctx_str (ctx, name);
  if (hard_reg_name != NULL) hard_reg_name = get_ctx_str (ctx, hard_reg_name);
  rd.hard_reg_name = (char *) hard_reg_name;
  rd.type = type;
  rd.reg = reg; /* 0 is reserved */
  rdn = VARR_LENGTH (reg_desc_t, func_regs->reg_descs);
  VARR_PUSH (reg_desc_t, func_regs->reg_descs, rd);
  if (HTAB_DO (size_t, func_regs->name2rdn_tab, rdn, HTAB_FIND, tab_rdn)) {
    VARR_POP (reg_desc_t, func_regs->reg_descs);
    MIR_get_error_func (ctx) (MIR_repeated_decl_error, "Repeated reg declaration %s", name);
  }
  if (hard_reg_name != NULL) {
    if ((hr = _MIR_get_hard_reg (ctx, hard_reg_name)) == MIR_NON_VAR) {
      MIR_get_error_func (ctx) (MIR_hard_reg_error, "unknown hard reg %s", hard_reg_name);
    } else if (!target_hard_reg_type_ok_p (hr, type)) {
      MIR_get_error_func (ctx) (MIR_hard_reg_error,
                                "reg %s tied to hard reg %s can not be of type %s", name,
                                hard_reg_name, MIR_type_str (ctx, type));
    } else if (target_fixed_hard_reg_p (hr)) {
      MIR_get_error_func (ctx) (MIR_hard_reg_error,
                                "reg %s can not be tied to reserved hard reg %s", name,
                                hard_reg_name);
    } else if (target_locs_num (hr, type) > 1)
      MIR_get_error_func (ctx) (MIR_hard_reg_error, "reg %s tied to %s requires more one hard reg",
                                name, hard_reg_name);
    if (HTAB_DO (size_t, func_regs->hrn2rdn_tab, rdn, HTAB_FIND, tab_rdn)) {
      rd_ref = &VARR_ADDR (reg_desc_t, func_regs->reg_descs)[tab_rdn];
      if (type != rd_ref->type)
        MIR_get_error_func (ctx) (MIR_repeated_decl_error,
                                  "regs %s and %s tied to hard reg %s have different types", name,
                                  rd_ref->name, hard_reg_name);
      /* Use always one reg for global vars assigned to hard regs: */
      VARR_POP (reg_desc_t, func_regs->reg_descs);
      *name_ptr = rd_ref->name;
      return rd_ref->reg;
    }
    func_module = func->func_item->module;
    if (func_module->data == NULL)
      func_module->data = bitmap_create2 (ctx->alloc, 128);
    bitmap_set_bit_p (func_module->data, hr); /* hard regs used for globals */
  }
  *name_ptr = rd.name;
  htab_res = HTAB_DO (size_t, func_regs->name2rdn_tab, rdn, HTAB_INSERT, tab_rdn);
  mir_assert (!htab_res);
  if (hard_reg_name != NULL) {
    htab_res = HTAB_DO (size_t, func_regs->hrn2rdn_tab, rdn, HTAB_INSERT, tab_rdn);
    mir_assert (!htab_res);
  }
  htab_res = HTAB_DO (size_t, func_regs->reg2rdn_tab, rdn, HTAB_INSERT, tab_rdn);
  mir_assert (!htab_res);
  return reg;
}

static void func_regs_finish (MIR_context_t ctx MIR_UNUSED, MIR_func_t func) {
  func_regs_t func_regs = func->internal;

  VARR_DESTROY (reg_desc_t, func_regs->reg_descs);
  HTAB_DESTROY (size_t, func_regs->name2rdn_tab);
  HTAB_DESTROY (size_t, func_regs->hrn2rdn_tab);
  HTAB_DESTROY (size_t, func_regs->reg2rdn_tab);
  MIR_free (ctx->alloc, func->internal);
  func->internal = NULL;
}

/* New Page */

static void push_data (MIR_context_t ctx, uint8_t *els, size_t size) {
  for (size_t i = 0; i < size; i++) VARR_PUSH (uint8_t, temp_data, els[i]);
}

const char *MIR_item_name (MIR_context_t ctx MIR_UNUSED, MIR_item_t item) {
  mir_assert (item != NULL);
  switch (item->item_type) {
  case MIR_func_item: return item->u.func->name;
  case MIR_proto_item: return item->u.proto->name;
  case MIR_import_item: return item->u.import_id;
  case MIR_export_item: return item->u.export_id;
  case MIR_forward_item: return item->u.forward_id;
  case MIR_bss_item: return item->u.bss->name;
  case MIR_data_item: return item->u.data->name;
  case MIR_ref_data_item: return item->u.ref_data->name;
  case MIR_lref_data_item: return item->u.lref_data->name;
  case MIR_expr_data_item: return item->u.expr_data->name;
  default: mir_assert (FALSE); return NULL;
  }
}

MIR_func_t MIR_get_item_func (MIR_context_t ctx MIR_UNUSED, MIR_item_t item) {
  mir_assert (item != NULL);
  if (item->item_type == MIR_func_item) {
    return item->u.func;
  } else {
    return NULL;
  }
}

#if !MIR_NO_IO
static void io_init (MIR_context_t ctx);
static void io_finish (MIR_context_t ctx);
#endif

#if !MIR_NO_SCAN
static void scan_init (MIR_context_t ctx);
static void scan_finish (MIR_context_t ctx);
#endif

static void simplify_init (MIR_context_t ctx);
static void simplify_finish (MIR_context_t ctx);

MIR_error_func_t MIR_get_error_func (MIR_context_t ctx) { return error_func; }  // ??? atomic

void MIR_set_error_func (MIR_context_t ctx, MIR_error_func_t func) {  // ?? atomic access
  error_func = func;
}

MIR_alloc_t MIR_get_alloc (MIR_context_t ctx) { return ctx->alloc; }

int MIR_get_func_redef_permission_p (MIR_context_t ctx) { return func_redef_permission_p; }

void MIR_set_func_redef_permission (MIR_context_t ctx, int enable_p) {  // ?? atomic access
  func_redef_permission_p = enable_p;
}

static htab_hash_t item_hash (MIR_item_t it, void *arg MIR_UNUSED) {
  return (htab_hash_t) mir_hash_finish (
    mir_hash_step (mir_hash_step (mir_hash_init (28), (uint64_t) MIR_item_name (NULL, it)),
                   (uint64_t) it->module));
}
static int item_eq (MIR_item_t it1, MIR_item_t it2, void *arg MIR_UNUSED) {
  return it1->module == it2->module && MIR_item_name (NULL, it1) == MIR_item_name (NULL, it2);
}

static MIR_item_t item_tab_find (MIR_context_t ctx, const char *name, MIR_module_t module) {
  MIR_item_t tab_item;
  struct MIR_item item_s;
  struct MIR_func func_s;

  item_s.item_type = MIR_func_item;
  func_s.name = name;
  item_s.module = module;
  item_s.u.func = &func_s;
  if (HTAB_DO (MIR_item_t, module_item_tab, &item_s, HTAB_FIND, tab_item)) return tab_item;
  return NULL;
}

static MIR_item_t item_tab_insert (MIR_context_t ctx, MIR_item_t item) {
  MIR_item_t tab_item;

  HTAB_DO (MIR_item_t, module_item_tab, item, HTAB_INSERT, tab_item);
  return tab_item;
}

static void item_tab_remove (MIR_context_t ctx, MIR_item_t item) {
  HTAB_DO (MIR_item_t, module_item_tab, item, HTAB_DELETE, item);
}

static void init_module (MIR_context_t ctx, MIR_module_t m, const char *name) {
  m->data = NULL;
  m->last_temp_item_num = 0;
  m->name = get_ctx_str (ctx, name);
  DLIST_INIT (MIR_item_t, m->items);
}

static void code_init (MIR_context_t ctx);
static void code_finish (MIR_context_t ctx);

double _MIR_get_api_version (void) { return MIR_API_VERSION; }

static void hard_reg_name_init (MIR_context_t ctx);
static void hard_reg_name_finish (MIR_context_t ctx);

#include "mir-alloc-default.c"
#include "mir-code-alloc-default.c"

MIR_context_t _MIR_init (MIR_alloc_t alloc, MIR_code_alloc_t code_alloc) {
  MIR_context_t ctx;

  if (alloc == NULL)
    alloc = &default_alloc;
  if (code_alloc == NULL)
    code_alloc = &default_code_alloc;

  mir_assert (MIR_OP_BOUND < OUT_FLAG);
  if ((ctx = MIR_malloc (alloc, sizeof (struct MIR_context))) == NULL)
    default_error (MIR_alloc_error, "Not enough memory for ctx");
  ctx->string_ctx = NULL;
  ctx->alias_ctx = NULL;
  ctx->reg_ctx = NULL;
  ctx->simplify_ctx = NULL;
  ctx->machine_code_ctx = NULL;
  ctx->io_ctx = NULL;
  ctx->scan_ctx = NULL;
  ctx->hard_reg_ctx = NULL;
  ctx->interp_ctx = NULL;
#ifndef NDEBUG
  for (MIR_insn_code_t c = 0; c < MIR_INVALID_INSN; c++) mir_assert (c == insn_descs[c].code);
#endif
  ctx->alloc = alloc;
  ctx->code_alloc = code_alloc;
  error_func = default_error;
  func_redef_permission_p = FALSE;
  curr_module = NULL;
  curr_func = NULL;
  curr_label_num = 0;
  if ((ctx->string_ctx = MIR_malloc (alloc, sizeof (struct string_ctx))) == NULL
      || (ctx->alias_ctx = MIR_malloc (alloc, sizeof (struct string_ctx))) == NULL)
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for ctx");
  string_init (alloc, &strings, &string_tab);
  string_init (alloc, &aliases, &alias_tab);
  VARR_CREATE (MIR_proto_t, unspec_protos, alloc, 0);
  check_and_prepare_insn_descs (ctx);
  DLIST_INIT (MIR_module_t, all_modules);
  simplify_init (ctx);
  VARR_CREATE (char, temp_string, alloc, 64);
  VARR_CREATE (uint8_t, temp_data, alloc, 512);
  VARR_CREATE (uint8_t, used_label_p, alloc, 512);
#if !MIR_NO_IO
  io_init (ctx);
#endif
#if !MIR_NO_SCAN
  scan_init (ctx);
#endif
  VARR_CREATE (MIR_module_t, modules_to_link, alloc, 0);
  VARR_CREATE (MIR_op_t, temp_ops, alloc, 0);
  init_module (ctx, &environment_module, ".environment");
  HTAB_CREATE (MIR_item_t, module_item_tab, ctx->alloc, 512, item_hash, item_eq, NULL);
  setjmp_addr = NULL;
  code_init (ctx);
  wrapper_end_addr = _MIR_get_wrapper_end (ctx); /* should be after code_init */
  hard_reg_name_init (ctx);
  interp_init (ctx);
  return ctx;
}

static void remove_insn (MIR_context_t ctx, MIR_item_t func_item, MIR_insn_t insn,
                         DLIST (MIR_insn_t) * insns) {
  mir_assert (func_item != NULL);
  if (func_item->item_type != MIR_func_item)
    MIR_get_error_func (ctx) (MIR_wrong_param_value_error, "MIR_remove_insn: wrong func item");
  DLIST_REMOVE (MIR_insn_t, *insns, insn);
  MIR_free (ctx->alloc, insn);
}

void MIR_remove_insn (MIR_context_t ctx, MIR_item_t func_item, MIR_insn_t insn) {
  remove_insn (ctx, func_item, insn, &func_item->u.func->insns);
}

static void remove_func_insns (MIR_context_t ctx, MIR_item_t func_item,
                               DLIST (MIR_insn_t) * insns) {
  MIR_insn_t insn;

  mir_assert (func_item->item_type == MIR_func_item);
  while ((insn = DLIST_HEAD (MIR_insn_t, *insns)) != NULL) {
    remove_insn (ctx, func_item, insn, insns);
  }
}

static void remove_item (MIR_context_t ctx, MIR_item_t item) {
  switch (item->item_type) {
  case MIR_func_item:
    remove_func_insns (ctx, item, &item->u.func->insns);
    remove_func_insns (ctx, item, &item->u.func->original_insns);
    VARR_DESTROY (MIR_var_t, item->u.func->vars);
    if (item->u.func->global_vars != NULL) VARR_DESTROY (MIR_var_t, item->u.func->global_vars);
    func_regs_finish (ctx, item->u.func);
    MIR_free (ctx->alloc, item->u.func);
    break;
  case MIR_proto_item:
    VARR_DESTROY (MIR_var_t, item->u.proto->args);
    MIR_free (ctx->alloc, item->u.proto);
    break;
  case MIR_import_item:
  case MIR_export_item:
  case MIR_forward_item: break;
  case MIR_data_item:
    if (item->addr != NULL && item->section_head_p)
      MIR_free (ctx->alloc, item->addr);
    MIR_free (ctx->alloc, item->u.data);
    break;
  case MIR_ref_data_item:
    if (item->addr != NULL && item->section_head_p)
      MIR_free (ctx->alloc, item->addr);
    MIR_free (ctx->alloc, item->u.ref_data);
    break;
  case MIR_lref_data_item:
    if (item->addr != NULL && item->section_head_p)
      MIR_free (ctx->alloc, item->addr);
    MIR_free (ctx->alloc, item->u.lref_data);
    break;
  case MIR_expr_data_item:
    if (item->addr != NULL && item->section_head_p)
      MIR_free (ctx->alloc, item->addr);
    MIR_free (ctx->alloc, item->u.expr_data);
    break;
  case MIR_bss_item:
    if (item->addr != NULL && item->section_head_p)
      MIR_free (ctx->alloc, item->addr);
    MIR_free (ctx->alloc, item->u.bss);
    break;
  default: mir_assert (FALSE);
  }
  if (item->data != NULL)
    MIR_free (ctx->alloc, item->data);
  MIR_free (ctx->alloc, item);
}

static void remove_module (MIR_context_t ctx, MIR_module_t module, int free_module_p) {
  MIR_item_t item;

  while ((item = DLIST_HEAD (MIR_item_t, module->items)) != NULL) {
    DLIST_REMOVE (MIR_item_t, module->items, item);
    remove_item (ctx, item);
  }
  if (module->data != NULL)
    bitmap_destroy (module->data);
  if (free_module_p)
    MIR_free (ctx->alloc, module);
}

static void remove_all_modules (MIR_context_t ctx) {
  MIR_module_t module;

  while ((module = DLIST_HEAD (MIR_module_t, all_modules)) != NULL) {
    DLIST_REMOVE (MIR_module_t, all_modules, module);
    remove_module (ctx, module, TRUE);
  }
  remove_module (ctx, &environment_module, FALSE);
}

void MIR_finish (MIR_context_t ctx) {
  interp_finish (ctx);
  remove_all_modules (ctx);
  HTAB_DESTROY (MIR_item_t, module_item_tab);
  VARR_DESTROY (MIR_module_t, modules_to_link);
  VARR_DESTROY (MIR_op_t, temp_ops);
#if !MIR_NO_SCAN
  scan_finish (ctx);
#endif
#if !MIR_NO_IO
  io_finish (ctx);
#endif
  VARR_DESTROY (uint8_t, temp_data);
  VARR_DESTROY (uint8_t, used_label_p);
  VARR_DESTROY (char, temp_string);
  while (VARR_LENGTH (MIR_proto_t, unspec_protos) != 0) {
    MIR_proto_t proto = VARR_POP (MIR_proto_t, unspec_protos);
    VARR_DESTROY (MIR_var_t, proto->args);
    MIR_free (ctx->alloc, proto);
  }
  VARR_DESTROY (MIR_proto_t, unspec_protos);
  string_finish (ctx->alloc, &strings, &string_tab);
  string_finish (ctx->alloc, &aliases, &alias_tab);
  simplify_finish (ctx);
  VARR_DESTROY (size_t, insn_nops);
  code_finish (ctx);
  hard_reg_name_finish (ctx);
  if (curr_func != NULL)
    MIR_get_error_func (ctx) (MIR_finish_error, "finish when function %s is not finished",
                              curr_func->name);
  if (curr_module != NULL)
    MIR_get_error_func (ctx) (MIR_finish_error, "finish when module %s is not finished",
                              curr_module->name);
  MIR_free (ctx->alloc, ctx->string_ctx);
  MIR_free (ctx->alloc, ctx->alias_ctx);
  MIR_free (ctx->alloc, ctx);
  ctx = NULL;
}

MIR_module_t MIR_new_module (MIR_context_t ctx, const char *name) {
  if (curr_module != NULL)
    MIR_get_error_func (ctx) (MIR_nested_module_error,
                              "Creating module when previous module %s is not finished",
                              curr_module->name);
  if ((curr_module = MIR_malloc (ctx->alloc, sizeof (struct MIR_module))) == NULL)
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for module %s creation", name);
  init_module (ctx, curr_module, name);
  DLIST_APPEND (MIR_module_t, all_modules, curr_module);
  return curr_module;
}

DLIST (MIR_module_t) * MIR_get_module_list (MIR_context_t ctx) { return &all_modules; }

static const char *type_str (MIR_context_t ctx, MIR_type_t tp) {
  int n;
  char str[100];

  switch (tp) {
  case MIR_T_I8: return "i8";
  case MIR_T_U8: return "u8";
  case MIR_T_I16: return "i16";
  case MIR_T_U16: return "u16";
  case MIR_T_I32: return "i32";
  case MIR_T_U32: return "u32";
  case MIR_T_I64: return "i64";
  case MIR_T_U64: return "u64";
  case MIR_T_F: return "f";
  case MIR_T_D: return "d";
  case MIR_T_LD: return "ld";
  case MIR_T_P: return "p";
  case MIR_T_RBLK: return "rblk";
  case MIR_T_UNDEF: return "undef";
  default:
    if (MIR_blk_type_p (tp) && (n = tp - MIR_T_BLK) >= 0 && n < MIR_BLK_NUM) {
      sprintf (str, "blk%d", n);
      return get_ctx_str (ctx, str);
    }
    return "";
  }
}

const char *MIR_type_str (MIR_context_t ctx, MIR_type_t tp) {
  const char *str = type_str (ctx, tp);

  if (strcmp (str, "") == 0)
    MIR_get_error_func (ctx) (MIR_wrong_param_value_error, "MIR_type_str: wrong type");
  return str;
}

static const char *mode_str (MIR_op_mode_t mode) {
  switch (mode) {
  case MIR_OP_REG: return "reg";
  case MIR_OP_VAR: return "var";
  case MIR_OP_INT: return "int";
  case MIR_OP_UINT: return "uint";
  case MIR_OP_FLOAT: return "float";
  case MIR_OP_DOUBLE: return "double";
  case MIR_OP_LDOUBLE: return "ldouble";
  case MIR_OP_REF: return "ref";
  case MIR_OP_STR: return "str";
  case MIR_OP_MEM: return "mem";
  case MIR_OP_VAR_MEM: return "var_mem";
  case MIR_OP_LABEL: return "label";
  case MIR_OP_BOUND: return "bound";
  case MIR_OP_UNDEF: return "undef";
  default: return "";
  }
}

static MIR_item_t add_item (MIR_context_t ctx, MIR_item_t item) {
  int replace_p;
  MIR_item_t tab_item;

  if ((tab_item = item_tab_find (ctx, MIR_item_name (ctx, item), item->module)) == NULL) {
    DLIST_APPEND (MIR_item_t, curr_module->items, item);
    HTAB_DO (MIR_item_t, module_item_tab, item, HTAB_INSERT, item);
    return item;
  }
  switch (tab_item->item_type) {
  case MIR_import_item:
    if (item->item_type != MIR_import_item)
      MIR_get_error_func (ctx) (MIR_import_export_error,
                                "existing module definition %s already defined as import",
                                tab_item->u.import_id);
    item = tab_item;
    break;
  case MIR_export_item:
  case MIR_forward_item:
    replace_p = FALSE;
    if (item->item_type == MIR_import_item) {
      MIR_get_error_func (ctx) (MIR_import_export_error, "export/forward of import %s",
                                item->u.import_id);
    } else if (item->item_type != MIR_export_item && item->item_type != MIR_forward_item) {
      replace_p = TRUE;
      DLIST_APPEND (MIR_item_t, curr_module->items, item);
    } else {
      if (tab_item->item_type == item->item_type)
        item = tab_item;
      else
        DLIST_APPEND (MIR_item_t, curr_module->items, item);
      if (item->item_type == MIR_export_item && tab_item->item_type == MIR_forward_item)
        replace_p = TRUE;
    }
    if (replace_p) { /* replace forward by export or export/forward by its definition: */
      tab_item->ref_def = item;
      if (tab_item->item_type == MIR_export_item) item->export_p = TRUE;
      item_tab_remove (ctx, tab_item);
      tab_item = item_tab_insert (ctx, item);
      mir_assert (item == tab_item);
    }
    break;
  case MIR_proto_item:
    MIR_get_error_func (ctx) (MIR_repeated_decl_error, "item %s was already defined as proto",
                              tab_item->u.proto->name);
    break;
  case MIR_bss_item:
  case MIR_data_item:
  case MIR_ref_data_item:
  case MIR_lref_data_item:
  case MIR_expr_data_item:
  case MIR_func_item:
    if (item->item_type == MIR_export_item) {
      if (tab_item->export_p) {
        item = tab_item;
      } else { /* just keep one export: */
        tab_item->export_p = TRUE;
        DLIST_APPEND (MIR_item_t, curr_module->items, item);
        item->ref_def = tab_item;
      }
    } else if (item->item_type == MIR_forward_item) {
      DLIST_APPEND (MIR_item_t, curr_module->items, item);
      item->ref_def = tab_item;
    } else if (item->item_type == MIR_import_item) {
      MIR_get_error_func (ctx) (MIR_import_export_error, "import of local definition %s",
                                item->u.import_id);
    } else {
      MIR_get_error_func (ctx) (MIR_repeated_decl_error, "Repeated item declaration %s",
                                MIR_item_name (ctx, item));
    }
    break;
  default: mir_assert (FALSE);
  }
  return item;
}

static MIR_item_t create_item (MIR_context_t ctx, MIR_item_type_t item_type,
                               const char *item_name) {
  MIR_item_t item;

  if (curr_module == NULL)
    MIR_get_error_func (ctx) (MIR_no_module_error, "%s outside module", item_name);
  if ((item = MIR_malloc (ctx->alloc, sizeof (struct MIR_item))) == NULL)
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for creation of item %s",
                              item_name);
  item->data = NULL;
  item->module = curr_module;
  item->item_type = item_type;
  item->ref_def = NULL;
  item->export_p = FALSE;
  item->section_head_p = FALSE;
  item->addr = NULL;
  return item;
}

static MIR_item_t new_export_import_forward (MIR_context_t ctx, const char *name,
                                             MIR_item_type_t item_type, const char *item_name,
                                             int create_only_p) {
  MIR_item_t item, tab_item;
  const char *uniq_name;

  item = create_item (ctx, item_type, item_name);
  uniq_name = get_ctx_str (ctx, name);
  if (item_type == MIR_export_item)
    item->u.export_id = uniq_name;
  else if (item_type == MIR_import_item)
    item->u.import_id = uniq_name;
  else
    item->u.forward_id = uniq_name;
  if (create_only_p) return item;
  if ((tab_item = add_item (ctx, item)) != item) {
    MIR_free (ctx->alloc, item);
    item = tab_item;
  }
  return item;
}

MIR_item_t MIR_new_export (MIR_context_t ctx, const char *name) {
  return new_export_import_forward (ctx, name, MIR_export_item, "export", FALSE);
}

MIR_item_t MIR_new_import (MIR_context_t ctx, const char *name) {
  return new_export_import_forward (ctx, name, MIR_import_item, "import", FALSE);
}

MIR_item_t MIR_new_forward (MIR_context_t ctx, const char *name) {
  return new_export_import_forward (ctx, name, MIR_forward_item, "forward", FALSE);
}

MIR_item_t MIR_new_bss (MIR_context_t ctx, const char *name, size_t len) {
  MIR_item_t tab_item, item = create_item (ctx, MIR_bss_item, "bss");

  item->u.bss = MIR_malloc (ctx->alloc, sizeof (struct MIR_bss));
  if (item->u.bss == NULL) {
    MIR_free (ctx->alloc, item);
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for creation of bss %s", name);
  }
  if (name != NULL) name = get_ctx_str (ctx, name);
  item->u.bss->name = name;
  item->u.bss->len = len;
  if (name == NULL) {
    DLIST_APPEND (MIR_item_t, curr_module->items, item);
  } else if ((tab_item = add_item (ctx, item)) != item) {
    MIR_free (ctx->alloc, item);
    item = tab_item;
  }
  return item;
}

static MIR_type_t canon_type (MIR_type_t type) {
#if defined(_WIN32) || __SIZEOF_LONG_DOUBLE__ == 8
  if (type == MIR_T_LD) type = MIR_T_D;
#endif
  return type;
}

size_t _MIR_type_size (MIR_context_t ctx MIR_UNUSED, MIR_type_t type) {
  switch (type) {
  case MIR_T_I8: return sizeof (int8_t);
  case MIR_T_U8: return sizeof (uint8_t);
  case MIR_T_I16: return sizeof (int16_t);
  case MIR_T_U16: return sizeof (uint16_t);
  case MIR_T_I32: return sizeof (int32_t);
  case MIR_T_U32: return sizeof (uint32_t);
  case MIR_T_I64: return sizeof (int64_t);
  case MIR_T_U64: return sizeof (uint64_t);
  case MIR_T_F: return sizeof (float);
  case MIR_T_D: return sizeof (double);
  case MIR_T_LD: return sizeof (long double);
  case MIR_T_P: return sizeof (void *);
  default: mir_assert (FALSE); return 1;
  }
}

static int wrong_type_p (MIR_type_t type) { return type < MIR_T_I8 || type >= MIR_T_BLK; }

MIR_item_t MIR_new_data (MIR_context_t ctx, const char *name, MIR_type_t el_type, size_t nel,
                         const void *els) {
  MIR_item_t tab_item, item = create_item (ctx, MIR_data_item, "data");
  MIR_data_t data;
  size_t el_len;

  if (wrong_type_p (el_type)) {
    MIR_free (ctx->alloc, item);
    MIR_get_error_func (ctx) (MIR_wrong_type_error, "wrong type in data %s", name);
  }
  el_len = _MIR_type_size (ctx, el_type);
  item->u.data = data = MIR_malloc (ctx->alloc, sizeof (struct MIR_data) + el_len * nel);
  if (data == NULL) {
    MIR_free (ctx->alloc, item);
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for creation of data %s",
                              name == NULL ? "" : name);
  }
  if (name != NULL) name = get_ctx_str (ctx, name);
  data->name = name;
  if (name == NULL) {
    DLIST_APPEND (MIR_item_t, curr_module->items, item);
  } else if ((tab_item = add_item (ctx, item)) != item) {
    MIR_free (ctx->alloc, item);
    item = tab_item;
  }
  data->el_type = canon_type (el_type);
  data->nel = nel;
  memcpy (data->u.els, els, el_len * nel);
  return item;
}

MIR_item_t MIR_new_string_data (MIR_context_t ctx, const char *name, MIR_str_t str) {
  return MIR_new_data (ctx, name, MIR_T_U8, str.len, str.s);
}

MIR_item_t MIR_new_ref_data (MIR_context_t ctx, const char *name, MIR_item_t ref_item,
                             int64_t disp) {
  MIR_item_t tab_item, item = create_item (ctx, MIR_ref_data_item, "ref data");
  MIR_ref_data_t ref_data;

  item->u.ref_data = ref_data = MIR_malloc (ctx->alloc, sizeof (struct MIR_ref_data));
  if (ref_data == NULL) {
    MIR_free (ctx->alloc, item);
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for creation of ref data %s",
                              name == NULL ? "" : name);
  }
  if (name != NULL) name = get_ctx_str (ctx, name);
  ref_data->name = name;
  ref_data->ref_item = ref_item;
  ref_data->disp = disp;
  if (name == NULL) {
    DLIST_APPEND (MIR_item_t, curr_module->items, item);
  } else if ((tab_item = add_item (ctx, item)) != item) {
    MIR_free (ctx->alloc, item);
    item = tab_item;
  }
  return item;
}

MIR_item_t MIR_new_lref_data (MIR_context_t ctx, const char *name, MIR_label_t label,
                              MIR_label_t label2, int64_t disp) {
  MIR_item_t tab_item, item = create_item (ctx, MIR_lref_data_item, "lref data");
  MIR_lref_data_t lref_data;

  if (label == NULL) {
    MIR_free (ctx->alloc, item);
    MIR_get_error_func (ctx) (MIR_alloc_error, "null label for lref data %s",
                              name == NULL ? "" : name);
  }
  item->u.lref_data = lref_data = MIR_malloc (ctx->alloc, sizeof (struct MIR_lref_data));
  if (lref_data == NULL) {
    MIR_free (ctx->alloc, item);
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for creation of lref data %s",
                              name == NULL ? "" : name);
  }
  if (name != NULL) name = get_ctx_str (ctx, name);
  lref_data->name = name;
  lref_data->label = label;
  lref_data->label2 = label2;
  lref_data->disp = disp;
  lref_data->orig_label = lref_data->orig_label2 = NULL;
  lref_data->next = NULL;
  if (name == NULL) {
    DLIST_APPEND (MIR_item_t, curr_module->items, item);
  } else if ((tab_item = add_item (ctx, item)) != item) {
    MIR_free (ctx->alloc, item);
    item = tab_item;
  }
  return item;
}

MIR_item_t MIR_new_expr_data (MIR_context_t ctx, const char *name, MIR_item_t expr_item) {
  MIR_item_t tab_item, item = create_item (ctx, MIR_expr_data_item, "expr data");
  MIR_expr_data_t expr_data;

  item->u.expr_data = expr_data = MIR_malloc (ctx->alloc, sizeof (struct MIR_expr_data));
  if (expr_data == NULL) {
    MIR_free (ctx->alloc, item);
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for creation of expr data %s",
                              name == NULL ? "" : name);
  }
  mir_assert (expr_item != NULL);
  if (expr_item->item_type != MIR_func_item || expr_item->u.func->vararg_p
      || expr_item->u.func->nargs != 0 || expr_item->u.func->nres != 1)
    MIR_get_error_func (
      ctx) (MIR_binary_io_error,
            "%s can not be an expr which should be non-argument, one result function",
            MIR_item_name (ctx, expr_item));
  if (name != NULL) name = get_ctx_str (ctx, name);
  expr_data->name = name;
  expr_data->expr_item = expr_item;
  if (name == NULL) {
    DLIST_APPEND (MIR_item_t, curr_module->items, item);
  } else if ((tab_item = add_item (ctx, item)) != item) {
    MIR_free (ctx->alloc, item);
    item = tab_item;
  }
  return item;
}

static MIR_proto_t create_proto (MIR_context_t ctx, const char *name, size_t nres,
                                 MIR_type_t *res_types, size_t nargs, int vararg_p,
                                 MIR_var_t *args) {
  MIR_proto_t proto = MIR_malloc (ctx->alloc, sizeof (struct MIR_proto) + nres * sizeof (MIR_type_t));
  MIR_var_t arg;

  if (proto == NULL)
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for creation of proto %s", name);
  proto->name = get_ctx_str (ctx, name);
  proto->res_types = (MIR_type_t *) ((char *) proto + sizeof (struct MIR_proto));
  if (nres != 0) memcpy (proto->res_types, res_types, nres * sizeof (MIR_type_t));
  proto->nres = (uint32_t) nres;
  proto->vararg_p = vararg_p != 0;
  VARR_CREATE (MIR_var_t, proto->args, ctx->alloc, nargs);
  for (size_t i = 0; i < nargs; i++) {
    arg = args[i];
    arg.name = get_ctx_str (ctx, arg.name);
    VARR_PUSH (MIR_var_t, proto->args, arg);
  }
  return proto;
}

static MIR_item_t new_proto_arr (MIR_context_t ctx, const char *name, size_t nres,
                                 MIR_type_t *res_types, size_t nargs, int vararg_p,
                                 MIR_var_t *args) {
  MIR_item_t proto_item, tab_item;

  if (curr_module == NULL)
    MIR_get_error_func (ctx) (MIR_no_module_error, "Creating proto %s outside module", name);
  for (size_t i = 0; i < nres; i++)
    if (wrong_type_p (res_types[i]))
      MIR_get_error_func (ctx) (MIR_wrong_type_error, "wrong result type in proto %s", name);
  proto_item = create_item (ctx, MIR_proto_item, "proto");
  proto_item->u.proto = create_proto (ctx, name, nres, res_types, nargs, vararg_p, args);
  tab_item = add_item (ctx, proto_item);
  mir_assert (tab_item == proto_item);
  return proto_item;
}

MIR_item_t MIR_new_proto_arr (MIR_context_t ctx, const char *name, size_t nres,
                              MIR_type_t *res_types, size_t nargs, MIR_var_t *args) {
  return new_proto_arr (ctx, name, nres, res_types, nargs, FALSE, args);
}

MIR_item_t MIR_new_vararg_proto_arr (MIR_context_t ctx, const char *name, size_t nres,
                                     MIR_type_t *res_types, size_t nargs, MIR_var_t *args) {
  return new_proto_arr (ctx, name, nres, res_types, nargs, TRUE, args);
}

#if defined(_MSC_VER)
#define alloca _alloca
#endif

static MIR_item_t new_proto (MIR_context_t ctx, const char *name, size_t nres,
                             MIR_type_t *res_types, size_t nargs, int vararg_p, va_list argp) {
  MIR_var_t *args = alloca (nargs * sizeof (MIR_var_t));
  size_t i;

  for (i = 0; i < nargs; i++) {
    args[i].type = va_arg (argp, MIR_type_t);
    args[i].name = va_arg (argp, const char *);
  }
  return new_proto_arr (ctx, name, nres, res_types, nargs, vararg_p, args);
}

MIR_item_t MIR_new_proto (MIR_context_t ctx, const char *name, size_t nres, MIR_type_t *res_types,
                          size_t nargs, ...) {
  va_list argp;
  MIR_item_t proto_item;

  va_start (argp, nargs);
  proto_item = new_proto (ctx, name, nres, res_types, nargs, FALSE, argp);
  va_end (argp);
  return proto_item;
}

MIR_item_t MIR_new_vararg_proto (MIR_context_t ctx, const char *name, size_t nres,
                                 MIR_type_t *res_types, size_t nargs, ...) {
  va_list argp;
  MIR_item_t proto_item;

  va_start (argp, nargs);
  proto_item = new_proto (ctx, name, nres, res_types, nargs, TRUE, argp);
  va_end (argp);
  return proto_item;
}

static MIR_item_t new_func_arr (MIR_context_t ctx, const char *name, size_t nres,
                                MIR_type_t *res_types, size_t nargs, int vararg_p,
                                MIR_var_t *vars) {
  MIR_item_t func_item, tab_item;
  MIR_func_t func;

  if (curr_func != NULL)
    MIR_get_error_func (ctx) (MIR_nested_func_error,
                              "Creating function when previous function %s is not finished",
                              curr_func->name);
  if (nargs == 0 && vararg_p)
    MIR_get_error_func (ctx) (MIR_vararg_func_error,
                              "Variable arg function %s w/o any mandatory argument", name);
  for (size_t i = 0; i < nres; i++)
    if (wrong_type_p (res_types[i]))
      MIR_get_error_func (ctx) (MIR_wrong_type_error, "wrong result type in func %s", name);
  func_item = create_item (ctx, MIR_func_item, "function");
  curr_func = func_item->u.func = func
    = MIR_malloc (ctx->alloc, sizeof (struct MIR_func) + nres * sizeof (MIR_type_t));
  if (func == NULL) {
    MIR_free (ctx->alloc, func_item);
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for creation of func %s", name);
  }
  func->name = get_ctx_str (ctx, name);
  func->func_item = func_item;
  func->nres = (uint32_t) nres;
  func->res_types = (MIR_type_t *) ((char *) func + sizeof (struct MIR_func));
  for (size_t i = 0; i < nres; i++) func->res_types[i] = canon_type (res_types[i]);
  tab_item = add_item (ctx, func_item);
  mir_assert (tab_item == func_item);
  DLIST_INIT (MIR_insn_t, func->insns);
  DLIST_INIT (MIR_insn_t, func->original_insns);
  VARR_CREATE (MIR_var_t, func->vars, ctx->alloc, nargs + 8);
  func->global_vars = NULL;
  func->nargs = (uint32_t) nargs;
  func->last_temp_num = 0;
  func->vararg_p = vararg_p != 0;
  func->expr_p = func->jret_p = FALSE;
  func->n_inlines = 0;
  func->machine_code = func->call_addr = NULL;
  func->first_lref = NULL;
  func_regs_init (ctx, func);
  for (size_t i = 0; i < nargs; i++) {
    char *stored_name;
    MIR_type_t type = canon_type (vars[i].type);
    MIR_reg_t reg
      = create_func_reg (ctx, func, vars[i].name, NULL, (MIR_reg_t) (i + 1),
                         type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD ? type : MIR_T_I64,
                         FALSE, &stored_name);
    mir_assert (i + 1 == reg);
    vars[i].name = stored_name;
    VARR_PUSH (MIR_var_t, func->vars, vars[i]);
  }
  return func_item;
}

MIR_item_t MIR_new_func_arr (MIR_context_t ctx, const char *name, size_t nres,
                             MIR_type_t *res_types, size_t nargs, MIR_var_t *vars) {
  return new_func_arr (ctx, name, nres, res_types, nargs, FALSE, vars);
}

MIR_item_t MIR_new_vararg_func_arr (MIR_context_t ctx, const char *name, size_t nres,
                                    MIR_type_t *res_types, size_t nargs, MIR_var_t *vars) {
  return new_func_arr (ctx, name, nres, res_types, nargs, TRUE, vars);
}

static MIR_item_t new_func (MIR_context_t ctx, const char *name, size_t nres, MIR_type_t *res_types,
                            size_t nargs, int vararg_p, va_list argp) {
  size_t i;
  MIR_var_t *vars = alloca (sizeof (MIR_var_t) * nargs);

  for (i = 0; i < nargs; i++) {
    vars[i].type = va_arg (argp, MIR_type_t);
    vars[i].name = va_arg (argp, const char *);
  }
  return new_func_arr (ctx, name, nres, res_types, nargs, vararg_p, vars);
}

MIR_item_t MIR_new_func (MIR_context_t ctx, const char *name, size_t nres, MIR_type_t *res_types,
                         size_t nargs, ...) {
  va_list argp;
  MIR_item_t func_item;

  va_start (argp, nargs);
  func_item = new_func (ctx, name, nres, res_types, nargs, FALSE, argp);
  va_end (argp);
  return func_item;
}

MIR_item_t MIR_new_vararg_func (MIR_context_t ctx, const char *name, size_t nres,
                                MIR_type_t *res_types, size_t nargs, ...) {
  va_list argp;
  MIR_item_t func_item;

  va_start (argp, nargs);
  func_item = new_func (ctx, name, nres, res_types, nargs, TRUE, argp);
  va_end (argp);
  return func_item;
}

static MIR_reg_t new_func_reg (MIR_context_t ctx, MIR_func_t func, MIR_type_t type,
                               const char *name, const char *hard_reg_name) {
  MIR_var_t var;
  MIR_reg_t res, reg;
  char *stored_name;

  if (func == NULL)
    MIR_get_error_func (ctx) (MIR_reg_type_error, "func can not be NULL for new reg creation");
  if (type != MIR_T_I64 && type != MIR_T_F && type != MIR_T_D && type != MIR_T_LD)
    MIR_get_error_func (ctx) (MIR_reg_type_error, "wrong type for var %s: got '%s'", name,
                              type_str (ctx, type));
  reg = (MIR_reg_t) VARR_LENGTH (MIR_var_t, func->vars) + 1;
  if (func->global_vars != NULL) reg += (MIR_reg_t) VARR_LENGTH (MIR_var_t, func->global_vars);
  res = create_func_reg (ctx, func, name, hard_reg_name, reg, type, FALSE, &stored_name);
  if (res != reg) return res; /* already exists */
  var.type = type;
  var.name = stored_name;
  if (hard_reg_name == NULL) {
    VARR_PUSH (MIR_var_t, func->vars, var);
  } else {
    if (func->global_vars == NULL) VARR_CREATE (MIR_var_t, func->global_vars, ctx->alloc, 8);
    VARR_PUSH (MIR_var_t, func->global_vars, var);
  }
  return res;
}

MIR_reg_t MIR_new_func_reg (MIR_context_t ctx, MIR_func_t func, MIR_type_t type, const char *name) {
  return new_func_reg (ctx, func, type, name, NULL);
}

MIR_reg_t MIR_new_global_func_reg (MIR_context_t ctx, MIR_func_t func, MIR_type_t type,
                                   const char *name, const char *hard_reg_name) {
  if (hard_reg_name == NULL)
    MIR_get_error_func (ctx) (MIR_hard_reg_error,
                              "global var %s should have non-null hard reg name", name);
  return new_func_reg (ctx, func, type, name, hard_reg_name);
}

static reg_desc_t *find_rd_by_name (MIR_context_t ctx MIR_UNUSED, const char *name,
                                    MIR_func_t func) {
  func_regs_t func_regs = func->internal;
  size_t rdn, temp_rdn;
  reg_desc_t rd;

  rd.name = (char *) name;
  rd.type = MIR_T_I64;
  rd.reg = 0; /* to eliminate warnings */
  temp_rdn = VARR_LENGTH (reg_desc_t, func_regs->reg_descs);
  VARR_PUSH (reg_desc_t, func_regs->reg_descs, rd);
  if (!HTAB_DO (size_t, func_regs->name2rdn_tab, temp_rdn, HTAB_FIND, rdn)) {
    VARR_POP (reg_desc_t, func_regs->reg_descs);
    return NULL; /* undeclared */
  }
  VARR_POP (reg_desc_t, func_regs->reg_descs);
  return &VARR_ADDR (reg_desc_t, func_regs->reg_descs)[rdn];
}

static reg_desc_t *find_rd_by_reg (MIR_context_t ctx, MIR_reg_t reg, MIR_func_t func) {
  func_regs_t func_regs = func->internal;
  size_t rdn, temp_rdn;
  reg_desc_t rd;

  rd.reg = reg;
  rd.name = NULL;
  rd.type = MIR_T_I64; /* to eliminate warnings */
  temp_rdn = VARR_LENGTH (reg_desc_t, func_regs->reg_descs);
  VARR_PUSH (reg_desc_t, func_regs->reg_descs, rd);
  if (!HTAB_DO (size_t, func_regs->reg2rdn_tab, temp_rdn, HTAB_FIND, rdn)) {
    VARR_POP (reg_desc_t, func_regs->reg_descs);
    MIR_get_error_func (ctx) (MIR_undeclared_func_reg_error, "undeclared reg %u of func %s", reg,
                              func->name);
  }
  VARR_POP (reg_desc_t, func_regs->reg_descs);
  return &VARR_ADDR (reg_desc_t, func_regs->reg_descs)[rdn];
}

void MIR_finish_func (MIR_context_t ctx) {
  int expr_p = TRUE;
  MIR_insn_t insn, prev_insn;
  MIR_insn_code_t code;
  const char *func_name;
  int ret_p = FALSE, jret_p = FALSE;

  if (curr_func == NULL)
    MIR_get_error_func (ctx) (MIR_no_func_error, "finish of non-existing function");
  func_name = curr_func->name;
  if (curr_func->vararg_p || curr_func->nargs != 0 || curr_func->nres != 1) expr_p = FALSE;
  for (insn = DLIST_HEAD (MIR_insn_t, curr_func->insns); insn != NULL;
       insn = DLIST_NEXT (MIR_insn_t, insn)) {
    size_t i, actual_nops = MIR_insn_nops (ctx, insn);
    MIR_op_mode_t mode, expected_mode;
    reg_desc_t *rd;
    int out_p, can_be_out_p;

    code = insn->code;
    if (code == MIR_RET) ret_p = TRUE;
    if (code == MIR_JRET) jret_p = TRUE;
    if (code == MIR_PHI || code == MIR_USE) {
      curr_func = NULL;
      MIR_get_error_func (ctx) (MIR_vararg_func_error, "use or phi can be used only internally");
    } else if (!curr_func->vararg_p && code == MIR_VA_START) {
      curr_func = NULL;
      MIR_get_error_func (ctx) (MIR_vararg_func_error, "va_start is not in vararg function");
    } else if (code == MIR_JRET && curr_func->nres != 0) {
      curr_func = NULL;
      MIR_get_error_func (
        ctx) (MIR_vararg_func_error,
              "func %s: in insn '%s': function should not have results in this case", func_name,
              insn_descs[code].name);
    } else if ((code == MIR_JRET && ret_p) || (code == MIR_RET && jret_p)) {
      curr_func = NULL;
      MIR_get_error_func (ctx) (MIR_vararg_func_error, "func %s: mix of RET and JRET insns",
                                func_name);
    } else if (code == MIR_RET && actual_nops != curr_func->nres) {
      curr_func = NULL;
      MIR_get_error_func (
        ctx) (MIR_vararg_func_error,
              "func %s: in instruction '%s': number of operands in return does not "
              "correspond number of function returns. Expected %d, got %d",
              func_name, insn_descs[code].name, curr_func->nres, actual_nops);
    } else if (MIR_call_code_p (code)) {
      expr_p = FALSE;
    } else if (code == MIR_BO || code == MIR_UBO || code == MIR_BNO || code == MIR_UBNO) {
      for (prev_insn = DLIST_PREV (MIR_insn_t, insn); prev_insn != NULL;
           prev_insn = DLIST_PREV (MIR_insn_t, prev_insn))
        if (prev_insn->code != MIR_MOV || prev_insn->ops[1].mode != MIR_OP_REG) break;
      if (prev_insn == NULL || !MIR_overflow_insn_code_p (prev_insn->code))
        MIR_get_error_func (ctx) (MIR_invalid_insn_error,
                                  "func %s: instruction '%s' has no previous overflow insn "
                                  "separated only by stores and reg moves",
                                  func_name, insn_descs[code].name);
      else if ((code == MIR_UBO || code == MIR_UBNO)
               && (prev_insn->code == MIR_MULO || prev_insn->code == MIR_MULOS))
        MIR_get_error_func (
          ctx) (MIR_invalid_insn_error,
                "func %s: unsigned overflow branch '%s' consumes flag of signed overflow insn '%s'",
                func_name, insn_descs[code].name, insn_descs[prev_insn->code].name);
      else if ((code == MIR_BO || code == MIR_BNO)
               && (prev_insn->code == MIR_UMULO || prev_insn->code == MIR_UMULOS))
        MIR_get_error_func (
          ctx) (MIR_invalid_insn_error,
                "func %s: signed overflow branch '%s' consumes flag of unsigned overflow insn '%s'",
                func_name, insn_descs[code].name, insn_descs[prev_insn->code].name);
    }
    for (i = 0; i < actual_nops; i++) {
      if (code == MIR_UNSPEC && i == 0) {
        mir_assert (insn->ops[i].mode == MIR_OP_INT);
        continue;
      } else if (MIR_call_code_p (code)) {
        if (i == 0) {
          mir_assert (insn->ops[i].mode == MIR_OP_REF
                      && insn->ops[i].u.ref->item_type == MIR_proto_item);
          continue; /* We checked the operand during insn creation -- skip the prototype */
        } else if (i == 1 && insn->ops[i].mode == MIR_OP_REF) {
          mir_assert (insn->ops[i].u.ref->item_type == MIR_import_item
                      || insn->ops[i].u.ref->item_type == MIR_export_item
                      || insn->ops[i].u.ref->item_type == MIR_forward_item
                      || insn->ops[i].u.ref->item_type == MIR_func_item);
          continue; /* We checked the operand during insn creation -- skip the func */
        }
      }
      if (code == MIR_VA_ARG && i == 2) {
        mir_assert (insn->ops[i].mode == MIR_OP_MEM);
        continue; /* We checked the operand during insn creation -- skip va_arg type  */
      }
      if (code == MIR_SWITCH) {
        out_p = FALSE;
        expected_mode = i == 0 ? MIR_OP_INT : MIR_OP_LABEL;
      } else if (code == MIR_RET) {
        out_p = FALSE;
        expected_mode = type2mode (curr_func->res_types[i]);
      } else {
        expected_mode = MIR_insn_op_mode (ctx, insn, i, &out_p);
      }
      can_be_out_p = TRUE;
      switch (insn->ops[i].mode) {
      case MIR_OP_REG:
        rd = find_rd_by_reg (ctx, insn->ops[i].u.reg, curr_func);
        mir_assert (rd != NULL && insn->ops[i].u.reg == rd->reg);
        mode = type2mode (rd->type);
        break;
      case MIR_OP_MEM:
        expr_p = FALSE;
        if (wrong_type_p (insn->ops[i].u.mem.type)
            && (!MIR_all_blk_type_p (insn->ops[i].u.mem.type) || !MIR_call_code_p (code))) {
          curr_func = NULL;
          MIR_get_error_func (ctx) (MIR_wrong_type_error,
                                    "func %s: in instruction '%s': wrong type memory", func_name,
                                    insn_descs[code].name);
        }
        if (MIR_all_blk_type_p (insn->ops[i].u.mem.type) && insn->ops[i].u.mem.disp < 0) {
          curr_func = NULL;
          MIR_get_error_func (ctx) (MIR_wrong_type_error,
                                    "func %s: in instruction '%s': block type memory with disp < 0",
                                    func_name, insn_descs[code].name);
        }
        if (insn->ops[i].u.mem.base != 0) {
          rd = find_rd_by_reg (ctx, insn->ops[i].u.mem.base, curr_func);
          mir_assert (rd != NULL && insn->ops[i].u.mem.base == rd->reg);
          if (type2mode (rd->type) != MIR_OP_INT) {
            curr_func = NULL;
            MIR_get_error_func (
              ctx) (MIR_reg_type_error,
                    "func %s: in instruction '%s': base reg of non-integer type for operand "
                    "#%d",
                    func_name, insn_descs[code].name, i + 1);
          }
        }
        if (insn->ops[i].u.mem.index != 0) {
          rd = find_rd_by_reg (ctx, insn->ops[i].u.mem.index, curr_func);
          mir_assert (rd != NULL && insn->ops[i].u.mem.index == rd->reg);
          if (type2mode (rd->type) != MIR_OP_INT) {
            curr_func = NULL;
            MIR_get_error_func (
              ctx) (MIR_reg_type_error,
                    "func %s: in instruction '%s': index reg of non-integer type for "
                    "operand #%d",
                    func_name, insn_descs[code].name, i + 1);
          }
        }
        mode = type2mode (insn->ops[i].u.mem.type);
        break;
      case MIR_OP_VAR:
      case MIR_OP_VAR_MEM:
        expr_p = FALSE;
        mode = expected_mode;
        mir_assert (FALSE); /* Should not be here */
        break;
      default:
        can_be_out_p = FALSE;
        mode = insn->ops[i].mode;
        if (mode == MIR_OP_REF || mode == MIR_OP_STR) mode = MIR_OP_INT; /* just an address */
        break;
      }
      insn->ops[i].value_mode = mode;
      if (mode == MIR_OP_UNDEF && insn->ops[i].mode == MIR_OP_MEM
          && ((code == MIR_VA_START && i == 0)
              || ((code == MIR_VA_ARG || code == MIR_VA_BLOCK_ARG) && i == 1)
              || (code == MIR_VA_END && i == 1))) { /* a special case: va_list as undef type mem */
        insn->ops[i].value_mode = expected_mode;
      } else if (expected_mode == MIR_OP_REG) {
        if (insn->ops[i].mode != MIR_OP_REG && insn->ops[i].mode != MIR_OP_VAR)
          MIR_get_error_func (
            ctx) (MIR_op_mode_error,
                  "func %s: in instruction '%s': expected reg for operand #%d. Got '%s'", func_name,
                  insn_descs[code].name, i + 1, mode_str (insn->ops[i].mode));
      } else if (expected_mode != MIR_OP_UNDEF
                 && (mode == MIR_OP_UINT ? MIR_OP_INT : mode) != expected_mode) {
        curr_func = NULL;
        MIR_get_error_func (
          ctx) (MIR_op_mode_error,
                "func %s: in instruction '%s': unexpected operand mode for operand #%d. Got "
                "'%s', expected '%s'",
                func_name, insn_descs[code].name, i + 1, mode_str (mode), mode_str (expected_mode));
      }
      if (out_p && !can_be_out_p) {
        curr_func = NULL;
        MIR_get_error_func (ctx) (MIR_out_op_error,
                                  "func %s; in instruction '%s': wrong operand #%d for insn output",
                                  func_name, insn_descs[code].name, i + 1);
      }
    }
  }
  if (!ret_p && !jret_p
      && ((insn = DLIST_TAIL (MIR_insn_t, curr_func->insns)) == NULL || insn->code != MIR_JMP)) {
    VARR_TRUNC (MIR_op_t, temp_ops, 0);
    for (size_t i = 0; i < curr_func->nres; i++) { /* add absent ret */
      MIR_op_t op;
      if (curr_func->res_types[i] == MIR_T_F)
        op = MIR_new_float_op (ctx, 0.0f);
      else if (curr_func->res_types[i] == MIR_T_D)
        op = MIR_new_double_op (ctx, 0.0);
      else if (curr_func->res_types[i] == MIR_T_LD)
        op = MIR_new_ldouble_op (ctx, 0.0);
      else
        op = MIR_new_int_op (ctx, 0);
      VARR_PUSH (MIR_op_t, temp_ops, op);
    }
    MIR_append_insn (ctx, curr_func->func_item,
                     MIR_new_insn_arr (ctx, MIR_RET, curr_func->nres,
                                       VARR_ADDR (MIR_op_t, temp_ops)));
  }
  curr_func->expr_p = expr_p;
  curr_func->jret_p = jret_p;
  curr_func = NULL;
}

void MIR_finish_module (MIR_context_t ctx) {
  if (curr_module == NULL)
    MIR_get_error_func (ctx) (MIR_no_module_error, "finish of non-existing module");
  curr_module = NULL;
}

static int setup_global (MIR_context_t ctx, const char *name, void *addr, MIR_item_t def) {
  MIR_item_t item, tab_item;
  MIR_module_t saved = curr_module;
  int redef_p = FALSE;

  curr_module = &environment_module;
  /* Use import for proto representation: */
  item = new_export_import_forward (ctx, name, MIR_import_item, "import", TRUE);
  if ((tab_item = item_tab_find (ctx, MIR_item_name (ctx, item), &environment_module)) != item
      && tab_item != NULL) {
    MIR_free (ctx->alloc, item);
    redef_p = TRUE;
  } else {
    HTAB_DO (MIR_item_t, module_item_tab, item, HTAB_INSERT, tab_item);
    DLIST_APPEND (MIR_item_t, environment_module.items, item);
    tab_item = item;
  }
  tab_item->addr = addr;
  tab_item->ref_def = def;
  curr_module = saved;
  return redef_p;
}

static void undefined_interface (MIR_context_t ctx) {
  MIR_get_error_func (ctx) (MIR_call_op_error, "undefined call interface");
}

static MIR_item_t load_bss_data_section (MIR_context_t ctx, MIR_item_t item, int first_only_p) {
  const char *name;
  MIR_item_t curr_item, last_item, expr_item;
  size_t len, section_size = 0;
  uint8_t *addr;

  if (item->addr == NULL) {
    /* Calculate section size: */
    for (curr_item = item; curr_item != NULL && curr_item->addr == NULL;
         curr_item = first_only_p ? NULL : DLIST_NEXT (MIR_item_t, curr_item))
      if (curr_item->item_type == MIR_bss_item
          && (curr_item == item || curr_item->u.bss->name == NULL))
        section_size += curr_item->u.bss->len;
      else if (curr_item->item_type == MIR_data_item
               && (curr_item == item || curr_item->u.data->name == NULL))
        section_size += (curr_item->u.data->nel * _MIR_type_size (ctx, curr_item->u.data->el_type));
      else if (curr_item->item_type == MIR_ref_data_item
               && (curr_item == item || curr_item->u.ref_data->name == NULL))
        section_size += _MIR_type_size (ctx, MIR_T_P);
      else if (curr_item->item_type == MIR_lref_data_item
               && (curr_item == item || curr_item->u.lref_data->name == NULL))
        section_size += _MIR_type_size (ctx, MIR_T_P);
      else if (curr_item->item_type == MIR_expr_data_item
               && (curr_item == item || curr_item->u.expr_data->name == NULL)) {
        expr_item = curr_item->u.expr_data->expr_item;
        if (expr_item->item_type != MIR_func_item || !expr_item->u.func->expr_p
            || expr_item->u.func->nres != 1)
          MIR_get_error_func (
            ctx) (MIR_binary_io_error,
                  "%s can not be an expr which should be a func w/o calls and memory ops",
                  MIR_item_name (ctx, expr_item));
        section_size += _MIR_type_size (ctx, expr_item->u.func->res_types[0]);
      } else
        break;
    if (section_size % 8 != 0)
      section_size += 8 - section_size % 8; /* we might use 64-bit copying of data */
    if ((item->addr = MIR_malloc (ctx->alloc, section_size)) == NULL) {
      name = MIR_item_name (ctx, item);
      MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory to allocate data/bss %s",
                                name == NULL ? "" : name);
    }
    item->section_head_p = TRUE;
  }
  /* Set up section memory: */
  for (last_item = item, curr_item = item, addr = item->addr;
       curr_item != NULL && (curr_item == item || curr_item->addr == NULL);
       last_item = curr_item, curr_item = first_only_p ? NULL : DLIST_NEXT (MIR_item_t, curr_item))
    if (curr_item->item_type == MIR_bss_item
        && (curr_item == item || curr_item->u.bss->name == NULL)) {
      memset (addr, 0, curr_item->u.bss->len);
      curr_item->addr = addr;
      addr += curr_item->u.bss->len;
    } else if (curr_item->item_type == MIR_data_item
               && (curr_item == item || curr_item->u.data->name == NULL)) {
      len = curr_item->u.data->nel * _MIR_type_size (ctx, curr_item->u.data->el_type);
      memmove (addr, curr_item->u.data->u.els, len);
      curr_item->addr = addr;
      addr += len;
    } else if (curr_item->item_type == MIR_ref_data_item
               && (curr_item == item || curr_item->u.ref_data->name == NULL)) {
      curr_item->u.ref_data->load_addr = addr;
      curr_item->addr = addr;
      addr += _MIR_type_size (ctx, MIR_T_P);
    } else if (curr_item->item_type == MIR_lref_data_item
               && (curr_item == item || curr_item->u.lref_data->name == NULL)) {
      curr_item->u.lref_data->load_addr = addr;
      curr_item->addr = addr;
      addr += _MIR_type_size (ctx, MIR_T_P);
    } else if (curr_item->item_type == MIR_expr_data_item
               && (curr_item == item || curr_item->u.expr_data->name == NULL)) {
      expr_item = curr_item->u.expr_data->expr_item;
      len = _MIR_type_size (ctx, expr_item->u.func->res_types[0]);
      curr_item->u.expr_data->load_addr = addr;
      curr_item->addr = addr;
      addr += len;
    } else {
      break;
    }
  return last_item;
}

static void link_module_lrefs (MIR_context_t ctx, MIR_module_t m) {
  for (MIR_item_t item = DLIST_HEAD (MIR_item_t, m->items); item != NULL;
       item = DLIST_NEXT (MIR_item_t, item)) {
    if (item->item_type != MIR_func_item) continue;
    for (MIR_insn_t insn = DLIST_HEAD (MIR_insn_t, item->u.func->insns); insn != NULL;
         insn = DLIST_NEXT (MIR_insn_t, insn))
      if (insn->code == MIR_LABEL) insn->data = item->u.func;
  }
  for (MIR_item_t item = DLIST_HEAD (MIR_item_t, m->items); item != NULL;
       item = DLIST_NEXT (MIR_item_t, item)) {
    if (item->item_type == MIR_lref_data_item) {
      MIR_lref_data_t lref_data = item->u.lref_data;
      MIR_label_t lab = lref_data->label, lab2 = lref_data->label2;
      MIR_func_t func = (MIR_func_t) lab->data;
      if (lab->data == NULL)
        MIR_get_error_func (ctx) (MIR_wrong_lref_error, "A label not from any function in lref %s",
                                  lref_data->name == NULL ? "" : lref_data->name);
      else if (lab2 != NULL && lab2->data != func)
        MIR_get_error_func (ctx) (MIR_wrong_lref_error,
                                  "Labels from different functions in lref %s",
                                  lref_data->name == NULL ? "" : lref_data->name);
      lref_data->next = func->first_lref;
      func->first_lref = lref_data;
    }
  }
  for (MIR_item_t item = DLIST_HEAD (MIR_item_t, m->items); item != NULL;
       item = DLIST_NEXT (MIR_item_t, item)) {
    if (item->item_type != MIR_func_item) continue;
    for (MIR_insn_t insn = DLIST_HEAD (MIR_insn_t, item->u.func->insns); insn != NULL;
         insn = DLIST_NEXT (MIR_insn_t, insn))
      if (insn->code == MIR_LABEL) insn->data = NULL;
  }
}

void MIR_load_module (MIR_context_t ctx, MIR_module_t m) {
  int lref_p = FALSE;
  mir_assert (m != NULL);
  for (MIR_item_t item = DLIST_HEAD (MIR_item_t, m->items); item != NULL;
       item = DLIST_NEXT (MIR_item_t, item)) {
    MIR_item_t first_item = item;

    if (item->item_type == MIR_bss_item || item->item_type == MIR_data_item
        || item->item_type == MIR_ref_data_item || item->item_type == MIR_lref_data_item
        || item->item_type == MIR_expr_data_item) {
      if (item->item_type == MIR_lref_data_item) lref_p = TRUE;
      item = load_bss_data_section (ctx, item, FALSE);
    } else if (item->item_type == MIR_func_item) {
      if (item->addr == NULL) {
        item->addr = _MIR_get_thunk (ctx);
#if defined(MIR_DEBUG)
        fprintf (stderr, "%016llx: %s\n", (unsigned long long) item->addr, item->u.func->name);
#endif
      }
      _MIR_redirect_thunk (ctx, item->addr, undefined_interface);
    }
    if (first_item->export_p) { /* update global item table */
      mir_assert (first_item->item_type != MIR_export_item
                  && first_item->item_type != MIR_import_item
                  && first_item->item_type != MIR_forward_item);
      if (setup_global (ctx, MIR_item_name (ctx, first_item), first_item->addr, first_item)
          && item->item_type == MIR_func_item
          && !func_redef_permission_p
#ifdef __APPLE__
          /* macosx can have multiple equal external inline definitions of the same function: */
          && strncmp (item->u.func->name, "__darwin", 8) != 0
#endif
      )
        MIR_get_error_func (ctx) (MIR_repeated_decl_error, "func %s is prohibited for redefinition",
                                  item->u.func->name);
    }
  }
  if (lref_p) link_module_lrefs (ctx, m);
  VARR_PUSH (MIR_module_t, modules_to_link, m);
}

#define SETJMP_NAME "setjmp"
#define SETJMP_NAME2 "_setjmp"

void MIR_load_external (MIR_context_t ctx, const char *name, void *addr) {
  if (strcmp (name, SETJMP_NAME) == 0 || (SETJMP_NAME2 != NULL && strcmp (name, SETJMP_NAME2) == 0))
    setjmp_addr = addr;
  setup_global (ctx, name, addr, NULL);
}

static void simplify_module_init (MIR_context_t ctx);
static int simplify_func (MIR_context_t ctx, MIR_item_t func_item, int mem_float_p);
static void process_inlines (MIR_context_t ctx, MIR_item_t func_item);

void MIR_link (MIR_context_t ctx, void (*set_interface) (MIR_context_t ctx, MIR_item_t item),
               void *import_resolver (const char *)) {
  MIR_item_t item, tab_item, expr_item;
  MIR_type_t type;
  MIR_val_t res;
  MIR_module_t m;
  void *addr;
  union {
    int8_t i8;
    int16_t i16;
    int32_t i32;
    int64_t i64;
    float f;
    double d;
    long double ld;
    void *a;
  } v;

  for (size_t i = 0; i < VARR_LENGTH (MIR_module_t, modules_to_link); i++) {
    m = VARR_GET (MIR_module_t, modules_to_link, i);
    simplify_module_init (ctx);
    for (item = DLIST_HEAD (MIR_item_t, m->items); item != NULL;
         item = DLIST_NEXT (MIR_item_t, item))
      if (item->item_type == MIR_func_item) {
        assert (item->data == NULL);
        if (simplify_func (ctx, item, TRUE)) item->data = (void *) 1; /* flag inlining */
      } else if (item->item_type == MIR_import_item) {
        if ((tab_item = item_tab_find (ctx, item->u.import_id, &environment_module)) == NULL) {
          if (import_resolver == NULL || (addr = import_resolver (item->u.import_id)) == NULL)
            MIR_get_error_func (ctx) (MIR_undeclared_op_ref_error, "import of undefined item %s",
                                      item->u.import_id);
          MIR_load_external (ctx, item->u.import_id, addr);
          tab_item = item_tab_find (ctx, item->u.import_id, &environment_module);
          mir_assert (tab_item != NULL);
        }
        item->addr = tab_item->addr;
        item->ref_def = tab_item;
      } else if (item->item_type == MIR_export_item) {
        if ((tab_item = item_tab_find (ctx, item->u.export_id, m)) == NULL)
          MIR_get_error_func (ctx) (MIR_undeclared_op_ref_error, "export of undefined item %s",
                                    item->u.export_id);
        item->addr = tab_item->addr;
        item->ref_def = tab_item;
      } else if (item->item_type == MIR_forward_item) {
        if ((tab_item = item_tab_find (ctx, item->u.forward_id, m)) == NULL)
          MIR_get_error_func (ctx) (MIR_undeclared_op_ref_error, "forward of undefined item %s",
                                    item->u.forward_id);
        item->addr = tab_item->addr;
        item->ref_def = tab_item;
      }
  }
  for (size_t i = 0; i < VARR_LENGTH (MIR_module_t, modules_to_link); i++) {
    m = VARR_GET (MIR_module_t, modules_to_link, i);
    for (item = DLIST_HEAD (MIR_item_t, m->items); item != NULL;
         item = DLIST_NEXT (MIR_item_t, item)) {
      if (item->item_type == MIR_func_item && item->data != NULL) {
        process_inlines (ctx, item);
        item->data = NULL;
#if 0
        fprintf (stderr, "+++++ Function after inlining:\n");
        MIR_output_item (ctx, stderr, item);
#endif
      } else if (item->item_type == MIR_ref_data_item) {
        assert (item->u.ref_data->ref_item->addr != NULL);
        addr = (char *) item->u.ref_data->ref_item->addr + item->u.ref_data->disp;
        memcpy (item->u.ref_data->load_addr, &addr, _MIR_type_size (ctx, MIR_T_P));
        continue;
      }
      /* lref data are set up in interpreter or generator */
      if (item->item_type != MIR_expr_data_item) continue;
      expr_item = item->u.expr_data->expr_item;
      MIR_interp (ctx, expr_item, &res, 0);
      type = expr_item->u.func->res_types[0];
      switch (type) {
      case MIR_T_I8:
      case MIR_T_U8: v.i8 = (int8_t) res.i; break;
      case MIR_T_I16:
      case MIR_T_U16: v.i16 = (int16_t) res.i; break;
      case MIR_T_I32:
      case MIR_T_U32: v.i32 = (int32_t) res.i; break;
      case MIR_T_I64:
      case MIR_T_U64: v.i64 = (int64_t) res.i; break;
      case MIR_T_F: v.f = res.f; break;
      case MIR_T_D: v.d = res.d; break;
      case MIR_T_LD: v.ld = res.ld; break;
      case MIR_T_P: v.a = res.a; break;
      default: assert (FALSE); break;
      }
      memcpy (item->u.expr_data->load_addr, &v,
              _MIR_type_size (ctx, expr_item->u.func->res_types[0]));
    }
  }
  if (set_interface != NULL) {
    while (VARR_LENGTH (MIR_module_t, modules_to_link) != 0) {
      m = VARR_POP (MIR_module_t, modules_to_link);
      for (item = DLIST_HEAD (MIR_item_t, m->items); item != NULL;
           item = DLIST_NEXT (MIR_item_t, item))
        if (item->item_type == MIR_func_item) {
          finish_func_interpretation (item, ctx->alloc); /* in case if it was used for expr data */
          set_interface (ctx, item);
        }
    }
    set_interface (ctx, NULL); /* finish interface setting */
  }
}

static const char *insn_name (MIR_insn_code_t code) {
  return (unsigned) code >= MIR_INSN_BOUND ? "" : insn_descs[code].name;
}

const char *MIR_insn_name (MIR_context_t ctx, MIR_insn_code_t code) {
  if ((unsigned) code >= MIR_INSN_BOUND)
    MIR_get_error_func (ctx) (MIR_wrong_param_value_error, "MIR_insn_name: wrong insn code %d",
                              (int) code);
  return insn_descs[code].name;
}

static size_t insn_code_nops (MIR_context_t ctx, MIR_insn_code_t code) { /* 0 for calls */
  if ((unsigned) code >= MIR_INSN_BOUND)
    MIR_get_error_func (ctx) (MIR_wrong_param_value_error, "insn_code_nops: wrong insn code %d",
                              (int) code);
  return VARR_GET (size_t, insn_nops, code);
}

size_t MIR_insn_nops (MIR_context_t ctx MIR_UNUSED, MIR_insn_t insn) {
  mir_assert (insn != NULL);
  return insn->nops;
}

MIR_op_mode_t _MIR_insn_code_op_mode (MIR_context_t ctx, MIR_insn_code_t code, size_t nop,
                                      int *out_p) {
  unsigned mode;
  mir_assert (out_p != NULL);

  if (nop >= insn_code_nops (ctx, code)) return MIR_OP_BOUND;
  mode = insn_descs[code].op_modes[nop];
  mir_assert (out_p != NULL);
  *out_p = (mode & OUT_FLAG) != 0;
  return *out_p ? mode ^ OUT_FLAG : mode;
}

MIR_op_mode_t MIR_insn_op_mode (MIR_context_t ctx, MIR_insn_t insn, size_t nop, int *out_p) {
  MIR_insn_code_t code = insn->code;
  size_t nargs, nops = MIR_insn_nops (ctx, insn);
  unsigned mode;

  *out_p = FALSE; /* to remove unitialized warning */
  if (nop >= nops) return MIR_OP_BOUND;
  mir_assert (out_p != NULL);
  switch (code) {
  case MIR_RET:
  case MIR_SWITCH:
    /* should be already checked in MIR_finish_func */
    return nop == 0 && code != MIR_RET ? MIR_OP_INT : insn->ops[nop].mode;
  case MIR_ADDR:
  case MIR_ADDR8:
  case MIR_ADDR16:
  case MIR_ADDR32: *out_p = nop == 0; return nop == 0 ? MIR_OP_INT : insn->ops[nop].mode;
  case MIR_PHI: *out_p = nop == 0; return insn->ops[nop].mode;
  case MIR_USE: return insn->ops[nop].mode;
  case MIR_CALL:
  case MIR_INLINE:
  case MIR_UNSPEC: {
    MIR_op_t proto_op;
    MIR_proto_t proto;
    size_t args_start;

    if (code == MIR_UNSPEC) {
      args_start = 1;
      mir_assert (insn->ops[0].mode == MIR_OP_INT);
      mir_assert (insn->ops[0].u.u < VARR_LENGTH (MIR_proto_t, unspec_protos));
      proto = VARR_GET (MIR_proto_t, unspec_protos, insn->ops[0].u.u);
    } else {
      args_start = 2;
      proto_op = insn->ops[0];
      mir_assert (proto_op.mode == MIR_OP_REF && proto_op.u.ref->item_type == MIR_proto_item);
      proto = proto_op.u.ref->u.proto;
    }
    *out_p = args_start <= nop && nop < proto->nres + args_start;
    nargs
      = proto->nres + args_start + (proto->args == NULL ? 0 : VARR_LENGTH (MIR_var_t, proto->args));
    if (proto->vararg_p && nop >= nargs) return MIR_OP_UNDEF; /* unknown */
    mir_assert (nops >= nargs && (proto->vararg_p || nops == nargs));
    if (nop == 0) return insn->ops[nop].mode;
    if (nop == 1 && code != MIR_UNSPEC) return MIR_OP_INT; /* call func addr */
    if (args_start <= nop && nop < proto->nres + args_start)
      return type2mode (proto->res_types[nop - args_start]);
    return type2mode (VARR_GET (MIR_var_t, proto->args, nop - args_start - proto->nres).type);
  }
  default:
    mode = insn_descs[code].op_modes[nop];
    if ((mode & OUT_FLAG) == 0) return mode;
    *out_p = TRUE;
    return mode ^ OUT_FLAG;
  }
}

static MIR_insn_t create_insn (MIR_context_t ctx, size_t nops, MIR_insn_code_t code) {
  MIR_insn_t insn;

  if (nops == 0) nops = 1;
  insn = MIR_malloc (ctx->alloc, sizeof (struct MIR_insn) + sizeof (MIR_op_t) * (nops - 1));
  if (insn == NULL)
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for insn creation");
#if defined(_WIN32) || __SIZEOF_LONG_DOUBLE__ == 8
  switch (code) {
  case MIR_LDMOV: code = MIR_DMOV; break;
  case MIR_I2LD: code = MIR_I2D; break;
  case MIR_UI2LD: code = MIR_UI2D; break;
  case MIR_LD2I: code = MIR_D2I; break;
  case MIR_F2LD: code = MIR_F2D; break;
  case MIR_D2LD: code = MIR_DMOV; break;
  case MIR_LD2F: code = MIR_D2F; break;
  case MIR_LD2D: code = MIR_DMOV; break;
  case MIR_LDNEG: code = MIR_DNEG; break;
  case MIR_LDADD: code = MIR_DADD; break;
  case MIR_LDSUB: code = MIR_DSUB; break;
  case MIR_LDMUL: code = MIR_DMUL; break;
  case MIR_LDDIV: code = MIR_DDIV; break;
  case MIR_LDEQ: code = MIR_DEQ; break;
  case MIR_LDNE: code = MIR_DNE; break;
  case MIR_LDLT: code = MIR_DLT; break;
  case MIR_LDLE: code = MIR_DLE; break;
  case MIR_LDGT: code = MIR_DGT; break;
  case MIR_LDGE: code = MIR_DGE; break;
  case MIR_LDBEQ: code = MIR_DBEQ; break;
  case MIR_LDBNE: code = MIR_DBNE; break;
  case MIR_LDBLT: code = MIR_DBLT; break;
  case MIR_LDBLE: code = MIR_DBLE; break;
  case MIR_LDBGT: code = MIR_DBGT; break;
  case MIR_LDBGE: code = MIR_DBGE; break;
  default: break;
  }
#endif
  insn->code = code;
  insn->data = NULL;
  return insn;
}

static MIR_insn_t new_insn1 (MIR_context_t ctx, MIR_insn_code_t code) {
  return create_insn (ctx, 1, code);
}

MIR_insn_t MIR_new_insn_arr (MIR_context_t ctx, MIR_insn_code_t code, size_t nops, MIR_op_t *ops) {
  MIR_insn_t insn;
  MIR_proto_t proto;
  size_t args_start, narg, i = 0, expected_nops = insn_code_nops (ctx, code);
  mir_assert (nops == 0 || ops != NULL);

  if (!MIR_call_code_p (code) && code != MIR_UNSPEC && code != MIR_USE && code != MIR_PHI
      && code != MIR_RET && code != MIR_SWITCH && nops != expected_nops) {
    MIR_get_error_func (ctx) (MIR_ops_num_error, "wrong number of operands for insn %s",
                              insn_descs[code].name);
  } else if (code == MIR_SWITCH) {
    if (nops < 2)
      MIR_get_error_func (ctx) (MIR_ops_num_error, "number of MIR_SWITCH operands is less 2");
  } else if (code == MIR_PHI) {
    if (nops < 3)
      MIR_get_error_func (ctx) (MIR_ops_num_error, "number of MIR_PHI operands is less 3");
  } else if (MIR_call_code_p (code) || code == MIR_UNSPEC) {
    args_start = code == MIR_UNSPEC ? 1 : 2;
    if (nops < args_start)
      MIR_get_error_func (ctx) (MIR_ops_num_error, "wrong number of call/unspec operands");
    if (code == MIR_UNSPEC) {
      if (ops[0].mode != MIR_OP_INT || ops[0].u.u >= VARR_LENGTH (MIR_proto_t, unspec_protos))
        MIR_get_error_func (ctx) (MIR_unspec_op_error,
                                  "the 1st unspec operand should be valid unspec code");
      proto = VARR_GET (MIR_proto_t, unspec_protos, ops[0].u.u);
    } else {
      if (ops[0].mode != MIR_OP_REF || ops[0].u.ref->item_type != MIR_proto_item)
        MIR_get_error_func (ctx) (MIR_call_op_error, "the 1st call operand should be a prototype");
      proto = ops[0].u.ref->u.proto;
    }
    i = proto->nres;
    if (proto->args != NULL) i += VARR_LENGTH (MIR_var_t, proto->args);
    if (nops < i + args_start || (nops != i + args_start && !proto->vararg_p))
      MIR_get_error_func (
        ctx) (code == MIR_UNSPEC ? MIR_unspec_op_error : MIR_call_op_error,
              "number of %s operands or results does not correspond to prototype %s",
              code == MIR_UNSPEC ? "unspec" : "call", proto->name);
    for (i = args_start; i < nops; i++) {
      if (ops[i].mode == MIR_OP_MEM && MIR_all_blk_type_p (ops[i].u.mem.type)) {
        if (i - args_start < proto->nres)
          MIR_get_error_func (ctx) (MIR_wrong_type_error, "result of %s is block type memory",
                                    code == MIR_UNSPEC ? "unspec" : "call");
        else if ((narg = i - args_start - proto->nres) < VARR_LENGTH (MIR_var_t, proto->args)) {
          if (VARR_GET (MIR_var_t, proto->args, narg).type != ops[i].u.mem.type) {
            MIR_get_error_func (
              ctx) (MIR_wrong_type_error,
                    "arg of %s is block type memory but param is not of block type",
                    code == MIR_UNSPEC ? "unspec" : "call");
          } else if (VARR_GET (MIR_var_t, proto->args, narg).size != (size_t) ops[i].u.mem.disp) {
            MIR_get_error_func (
              ctx) (MIR_wrong_type_error,
                    "different sizes (%lu vs %lu) of arg and param block memory in %s insn",
                    (unsigned long) VARR_GET (MIR_var_t, proto->args, narg).size,
                    (unsigned long) ops[i].u.mem.disp, code == MIR_UNSPEC ? "unspec" : "call");
          }
        } else if (ops[i].u.mem.type == MIR_T_RBLK) {
          MIR_get_error_func (ctx) (MIR_wrong_type_error,
                                    "RBLK memory can not correspond to unnamed param in %s insn",
                                    code == MIR_UNSPEC ? "unspec" : "call");
        }
      } else if (i - args_start >= proto->nres
                 && (narg = i - args_start - proto->nres) < VARR_LENGTH (MIR_var_t, proto->args)
                 && MIR_all_blk_type_p (VARR_GET (MIR_var_t, proto->args, narg).type)) {
        MIR_get_error_func (
          ctx) (MIR_wrong_type_error,
                "param of %s is of block type but arg is not of block type memory",
                code == MIR_UNSPEC ? "unspec" : "call");
      }
    }
  } else if (code == MIR_VA_ARG) {  // undef mem ???
    if (ops[2].mode != MIR_OP_MEM)
      MIR_get_error_func (ctx) (MIR_op_mode_error,
                                "3rd operand of va_arg should be any memory with given type");
  } else if (code == MIR_PRSET) {
    if (ops[1].mode != MIR_OP_INT)
      MIR_get_error_func (ctx) (MIR_op_mode_error, "property should be a integer operand");
  } else if (code == MIR_PRBEQ || code == MIR_PRBNE) {
    if (ops[2].mode != MIR_OP_INT)
      MIR_get_error_func (ctx) (MIR_op_mode_error, "property should be a integer operand");
    if (ops[1].mode != MIR_OP_REG && ops[1].mode != MIR_OP_MEM)
      MIR_get_error_func (
        ctx) (MIR_op_mode_error,
              "2nd operand of property branch should be any memory or reg with given type");
  }
  insn = create_insn (ctx, nops, code);
  insn->nops = (unsigned int) nops;
  for (i = 0; i < nops; i++) insn->ops[i] = ops[i];
  return insn;
}

static MIR_insn_t new_insn (MIR_context_t ctx, MIR_insn_code_t code, size_t nops, va_list argp) {
  MIR_op_t *insn_ops = alloca (sizeof (MIR_op_t) * nops);

  for (size_t i = 0; i < nops; i++) insn_ops[i] = va_arg (argp, MIR_op_t);
  va_end (argp);
  return MIR_new_insn_arr (ctx, code, nops, insn_ops);
}

MIR_insn_t MIR_new_insn (MIR_context_t ctx, MIR_insn_code_t code, ...) {
  va_list argp;
  size_t nops = insn_code_nops (ctx, code);

  if (code == MIR_USE || code == MIR_PHI)
    MIR_get_error_func (ctx) (MIR_call_op_error,
                              "Use only MIR_new_insn_arr for creating use or phi insn");
  else if (MIR_call_code_p (code) || code == MIR_UNSPEC || code == MIR_RET || code == MIR_SWITCH)
    MIR_get_error_func (
      ctx) (MIR_call_op_error,
            "Use only MIR_new_insn_arr or MIR_new_{call,unspec,ret}_insn for creating a "
            "call/unspec/ret/jret/switch insn");
  va_start (argp, code);
  return new_insn (ctx, code, nops, argp);
}

MIR_insn_t MIR_new_call_insn (MIR_context_t ctx, size_t nops, ...) {
  va_list argp;

  va_start (argp, nops);
  return new_insn (ctx, MIR_CALL, nops, argp);
}

MIR_insn_t MIR_new_jcall_insn (MIR_context_t ctx, size_t nops, ...) {
  va_list argp;

  va_start (argp, nops);
  return new_insn (ctx, MIR_JCALL, nops, argp);
}

MIR_insn_t MIR_new_ret_insn (MIR_context_t ctx, size_t nops, ...) {
  va_list argp;

  va_start (argp, nops);
  return new_insn (ctx, MIR_RET, nops, argp);
}

MIR_insn_t _MIR_new_unspec_insn (MIR_context_t ctx, size_t nops, ...) {
  va_list argp;

  va_start (argp, nops);
  return new_insn (ctx, MIR_UNSPEC, nops, argp);
}

void _MIR_register_unspec_insn (MIR_context_t ctx, uint64_t code, const char *name, size_t nres,
                                MIR_type_t *res_types, size_t nargs, int vararg_p,
                                MIR_var_t *args) {
  MIR_proto_t proto;

  while (VARR_LENGTH (MIR_proto_t, unspec_protos) <= code)
    VARR_PUSH (MIR_proto_t, unspec_protos, NULL);
  if ((proto = VARR_GET (MIR_proto_t, unspec_protos, code)) == NULL) {
    VARR_SET (MIR_proto_t, unspec_protos, code,
              create_proto (ctx, name, nres, res_types, nargs, vararg_p, args));
  } else {
    assert (strcmp (proto->name, name) == 0);
  }
}

MIR_insn_t MIR_copy_insn (MIR_context_t ctx, MIR_insn_t insn) {
  size_t size;
  mir_assert (insn != NULL);
  size = sizeof (struct MIR_insn) + sizeof (MIR_op_t) * (insn->nops == 0 ? 0 : insn->nops - 1);
  MIR_insn_t new_insn = MIR_malloc (ctx->alloc, size);

  if (new_insn == NULL)
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory to copy insn %s",
                              insn_name (insn->code));
  memcpy (new_insn, insn, size);
  return new_insn;
}

static MIR_insn_t create_label (MIR_context_t ctx, int64_t label_num) {
  MIR_insn_t insn = new_insn1 (ctx, MIR_LABEL);

  insn->ops[0] = MIR_new_int_op (ctx, label_num);
  insn->nops = 0;
  return insn;
}

MIR_insn_t MIR_new_label (MIR_context_t ctx) { return create_label (ctx, ++curr_label_num); }

void _MIR_free_insn (MIR_context_t ctx MIR_UNUSED, MIR_insn_t insn) {
  MIR_free (ctx->alloc, insn);
}

static MIR_reg_t new_temp_reg (MIR_context_t ctx, MIR_type_t type, MIR_func_t func) {
  char reg_name[30];

  if (type != MIR_T_I64 && type != MIR_T_F && type != MIR_T_D && type != MIR_T_LD)
    MIR_get_error_func (ctx) (MIR_reg_type_error, "wrong type %s for temporary register",
                              type_str (ctx, type));
  mir_assert (func != NULL);
  for (;;) {
    func->last_temp_num++;
    if (func->last_temp_num == 0)
      MIR_get_error_func (ctx) (MIR_unique_reg_error, "out of unique regs");
    sprintf (reg_name, "%s%d", TEMP_REG_NAME_PREFIX, func->last_temp_num);

    if (find_rd_by_name (ctx, reg_name, func) == NULL)
      return MIR_new_func_reg (ctx, func, type, reg_name);
  }
}

MIR_reg_t _MIR_new_temp_reg (MIR_context_t ctx, MIR_type_t type, MIR_func_t func) {
  return new_temp_reg (ctx, type, func);
}

static reg_desc_t *get_func_rd_by_name (MIR_context_t ctx, const char *reg_name, MIR_func_t func) {
  reg_desc_t *rd;

  rd = find_rd_by_name (ctx, reg_name, func);
  if (rd == NULL)
    MIR_get_error_func (ctx) (MIR_undeclared_func_reg_error, "undeclared func reg %s", reg_name);
  return rd;
}

static reg_desc_t *get_func_rd_by_reg (MIR_context_t ctx, MIR_reg_t reg, MIR_func_t func) {
  reg_desc_t *rd;

  rd = find_rd_by_reg (ctx, reg, func);
  return rd;
}

MIR_reg_t MIR_reg (MIR_context_t ctx, const char *reg_name, MIR_func_t func) {
  return get_func_rd_by_name (ctx, reg_name, func)->reg;
}

MIR_type_t MIR_reg_type (MIR_context_t ctx, MIR_reg_t reg, MIR_func_t func) {
  return get_func_rd_by_reg (ctx, reg, func)->type;
}

const char *MIR_reg_name (MIR_context_t ctx, MIR_reg_t reg, MIR_func_t func) {
  return get_func_rd_by_reg (ctx, reg, func)->name;
}

const char *MIR_reg_hard_reg_name (MIR_context_t ctx, MIR_reg_t reg, MIR_func_t func) {
  const reg_desc_t *rd = get_func_rd_by_reg (ctx, reg, func);
  return rd->hard_reg_name;
}

/* Functions to create operands.  */

static void init_op (MIR_op_t *op, MIR_op_mode_t mode) {
  op->mode = mode;
  op->data = NULL;
}

MIR_op_t MIR_new_reg_op (MIR_context_t ctx MIR_UNUSED, MIR_reg_t reg) {
  MIR_op_t op;

  init_op (&op, MIR_OP_REG);
  op.u.reg = reg;
  return op;
}

MIR_op_t _MIR_new_var_op (MIR_context_t ctx MIR_UNUSED, MIR_reg_t var) { /* used only internally */
  MIR_op_t op;

  init_op (&op, MIR_OP_VAR);
  op.u.var = var;
  return op;
}

MIR_op_t MIR_new_int_op (MIR_context_t ctx MIR_UNUSED, int64_t i) {
  MIR_op_t op;

  init_op (&op, MIR_OP_INT);
  op.u.i = i;
  return op;
}

MIR_op_t MIR_new_uint_op (MIR_context_t ctx MIR_UNUSED, uint64_t u) {
  MIR_op_t op;

  init_op (&op, MIR_OP_UINT);
  op.u.u = u;
  return op;
}

MIR_op_t MIR_new_float_op (MIR_context_t ctx MIR_UNUSED, float f) {
  MIR_op_t op;

  mir_assert (sizeof (float) == 4); /* IEEE single */
  init_op (&op, MIR_OP_FLOAT);
  op.u.f = f;
  return op;
}

MIR_op_t MIR_new_double_op (MIR_context_t ctx MIR_UNUSED, double d) {
  MIR_op_t op;

  mir_assert (sizeof (double) == 8); /* IEEE double */
  init_op (&op, MIR_OP_DOUBLE);
  op.u.d = d;
  return op;
}

MIR_op_t MIR_new_ldouble_op (MIR_context_t ctx MIR_UNUSED, long double ld) {
  MIR_op_t op;

#if defined(_WIN32) || __SIZEOF_LONG_DOUBLE__ == 8
  return MIR_new_double_op (ctx, ld);
#endif
  mir_assert (sizeof (long double) == 16); /* machine-defined 80- or 128-bit FP  */
  init_op (&op, MIR_OP_LDOUBLE);
  op.u.ld = ld;
  return op;
}

MIR_op_t MIR_new_ref_op (MIR_context_t ctx MIR_UNUSED, MIR_item_t item) {
  MIR_op_t op;

  init_op (&op, MIR_OP_REF);
  op.u.ref = item;
  return op;
}

MIR_op_t MIR_new_str_op (MIR_context_t ctx, MIR_str_t str) {
  MIR_op_t op;

  init_op (&op, MIR_OP_STR);
  op.u.str = get_ctx_string (ctx, str).str;
  return op;
}

static MIR_op_t new_mem_op (MIR_context_t ctx MIR_UNUSED, MIR_type_t type, MIR_disp_t disp,
                            MIR_reg_t base, MIR_reg_t index, MIR_scale_t scale, MIR_alias_t alias,
                            MIR_alias_t nonalias) {
  MIR_op_t op;

  init_op (&op, MIR_OP_MEM);
  op.u.mem.type = canon_type (type);
  op.u.mem.disp = disp;
  op.u.mem.base = base;
  op.u.mem.index = index;
  op.u.mem.scale = scale;
  op.u.mem.nloc = 0;
  op.u.mem.alias = alias;
  op.u.mem.nonalias = nonalias;
  return op;
}

MIR_op_t MIR_new_mem_op (MIR_context_t ctx, MIR_type_t type, MIR_disp_t disp, MIR_reg_t base,
                         MIR_reg_t index, MIR_scale_t scale) {
  return new_mem_op (ctx, type, disp, base, index, scale, 0, 0);
}

MIR_op_t MIR_new_alias_mem_op (MIR_context_t ctx, MIR_type_t type, MIR_disp_t disp, MIR_reg_t base,
                               MIR_reg_t index, MIR_scale_t scale, MIR_alias_t alias,
                               MIR_alias_t nonalias) {
  return new_mem_op (ctx, type, disp, base, index, scale, alias, nonalias);
}

static MIR_op_t new_var_mem_op (MIR_context_t ctx MIR_UNUSED, MIR_type_t type, MIR_disp_t disp,
                                MIR_reg_t base, MIR_reg_t index, MIR_scale_t scale,
                                MIR_alias_t alias, MIR_alias_t nonalias) {
  MIR_op_t op;

  init_op (&op, MIR_OP_VAR_MEM);
  op.u.var_mem.type = type;
  op.u.var_mem.disp = disp;
  op.u.var_mem.base = base;
  op.u.var_mem.index = index;
  op.u.var_mem.scale = scale;
  op.u.var_mem.nloc = 0;
  op.u.var_mem.alias = alias;
  op.u.var_mem.nonalias = nonalias;
  return op;
}

MIR_op_t _MIR_new_var_mem_op (MIR_context_t ctx, MIR_type_t type, MIR_disp_t disp, MIR_reg_t base,
                              MIR_reg_t index, MIR_scale_t scale) {
  return new_var_mem_op (ctx, type, disp, base, index, scale, 0, 0);
}

MIR_op_t _MIR_new_alias_var_mem_op (MIR_context_t ctx, MIR_type_t type, MIR_disp_t disp,
                                    MIR_reg_t base, MIR_reg_t index, MIR_scale_t scale,
                                    MIR_alias_t alias, MIR_alias_t nonalias) {
  return new_var_mem_op (ctx, type, disp, base, index, scale, alias, nonalias);
}

MIR_op_t MIR_new_label_op (MIR_context_t ctx MIR_UNUSED, MIR_label_t label) {
  MIR_op_t op;

  init_op (&op, MIR_OP_LABEL);
  op.u.label = label;
  return op;
}

int MIR_op_eq_p (MIR_context_t ctx, MIR_op_t op1, MIR_op_t op2) {
  if (op1.mode != op2.mode) return FALSE;
  switch (op1.mode) {
  case MIR_OP_REG: return op1.u.reg == op2.u.reg;
  case MIR_OP_VAR: return op1.u.var == op2.u.var;
  case MIR_OP_INT: return op1.u.i == op2.u.i;
  case MIR_OP_UINT: return op1.u.u == op2.u.u;
  case MIR_OP_FLOAT: return op1.u.f == op2.u.f;
  case MIR_OP_DOUBLE: return op1.u.d == op2.u.d;
  case MIR_OP_LDOUBLE: return op1.u.ld == op2.u.ld;
  case MIR_OP_REF:
    if (op1.u.ref->item_type == MIR_export_item || op1.u.ref->item_type == MIR_import_item)
      return strcmp (MIR_item_name (ctx, op1.u.ref), MIR_item_name (ctx, op2.u.ref)) == 0;
    return op1.u.ref == op2.u.ref;
  case MIR_OP_STR:
    return op1.u.str.len == op2.u.str.len && memcmp (op1.u.str.s, op2.u.str.s, op1.u.str.len) == 0;
  case MIR_OP_MEM:
    return (op1.u.mem.type == op2.u.mem.type && op1.u.mem.disp == op2.u.mem.disp
            && op1.u.mem.base == op2.u.mem.base && op1.u.mem.index == op2.u.mem.index
            && (op1.u.mem.index == 0 || op1.u.mem.scale == op2.u.mem.scale));
  case MIR_OP_VAR_MEM:
    return (op1.u.var_mem.type == op2.u.var_mem.type && op1.u.var_mem.disp == op2.u.var_mem.disp
            && op1.u.var_mem.base == op2.u.var_mem.base
            && op1.u.var_mem.index == op2.u.var_mem.index
            && (op1.u.var_mem.index == MIR_NON_VAR || op1.u.var_mem.scale == op2.u.var_mem.scale));
  case MIR_OP_LABEL: return op1.u.label == op2.u.label;
  default: mir_assert (FALSE); /* we should not have other operands here */
  }
  return FALSE;
}

htab_hash_t MIR_op_hash_step (MIR_context_t ctx, htab_hash_t h, MIR_op_t op) {
  h = (htab_hash_t) mir_hash_step (h, (uint64_t) op.mode);
  switch (op.mode) {
  case MIR_OP_REG: return (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.reg);
  case MIR_OP_VAR: return (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.var);
  case MIR_OP_INT: return (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.i);
  case MIR_OP_UINT: return (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.u);
  case MIR_OP_FLOAT: {
    union {
      double d;
      uint64_t u;
    } u;

    u.d = op.u.f;
    return (htab_hash_t) mir_hash_step (h, u.u);
  }
  case MIR_OP_DOUBLE: return (htab_hash_t) mir_hash_step (h, op.u.u);
  case MIR_OP_LDOUBLE: {
    union {
      long double ld;
      uint64_t u[2];
    } u;

    u.ld = op.u.ld;
    return (htab_hash_t) mir_hash_step (mir_hash_step (h, u.u[0]), u.u[1]);
  }
  case MIR_OP_REF:
    if (op.u.ref->item_type == MIR_export_item || op.u.ref->item_type == MIR_import_item)
      return (htab_hash_t) mir_hash_step (h, (uint64_t) MIR_item_name (ctx, op.u.ref));
    return (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.ref);
  case MIR_OP_STR: return (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.str.s);
  case MIR_OP_MEM:
    h = (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.mem.type);
    h = (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.mem.disp);
    h = (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.mem.base);
    h = (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.mem.index);
    if (op.u.mem.index != 0) h = (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.mem.scale);
    break;
  case MIR_OP_VAR_MEM:
    h = (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.var_mem.type);
    h = (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.var_mem.disp);
    h = (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.var_mem.base);
    h = (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.var_mem.index);
    if (op.u.var_mem.index != MIR_NON_VAR)
      h = (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.var_mem.scale);
    break;
  case MIR_OP_LABEL: return (htab_hash_t) mir_hash_step (h, (uint64_t) op.u.label);
  default: mir_assert (FALSE); /* we should not have other operands here */
  }
  return h;
}

void MIR_append_insn (MIR_context_t ctx, MIR_item_t func_item, MIR_insn_t insn) {
  mir_assert (func_item != NULL);
  if (func_item->item_type != MIR_func_item)
    MIR_get_error_func (ctx) (MIR_wrong_param_value_error, "MIR_append_insn: wrong func item");
  DLIST_APPEND (MIR_insn_t, func_item->u.func->insns, insn);
}

void MIR_prepend_insn (MIR_context_t ctx, MIR_item_t func_item, MIR_insn_t insn) {
  mir_assert (func_item != NULL);
  if (func_item->item_type != MIR_func_item)
    MIR_get_error_func (ctx) (MIR_wrong_param_value_error, "MIR_prepend_insn: wrong func item");
  DLIST_PREPEND (MIR_insn_t, func_item->u.func->insns, insn);
}

void MIR_insert_insn_after (MIR_context_t ctx, MIR_item_t func_item, MIR_insn_t after,
                            MIR_insn_t insn) {
  mir_assert (func_item != NULL);
  if (func_item->item_type != MIR_func_item)
    MIR_get_error_func (ctx) (MIR_wrong_param_value_error,
                              "MIR_insert_insn_after: wrong func item");
  DLIST_INSERT_AFTER (MIR_insn_t, func_item->u.func->insns, after, insn);
}

void MIR_insert_insn_before (MIR_context_t ctx, MIR_item_t func_item, MIR_insn_t before,
                             MIR_insn_t insn) {
  mir_assert (func_item != NULL);
  if (func_item->item_type != MIR_func_item)
    MIR_get_error_func (ctx) (MIR_wrong_param_value_error,
                              "MIR_insert_insn_before: wrong func item");
  DLIST_INSERT_BEFORE (MIR_insn_t, func_item->u.func->insns, before, insn);
}

static void store_labels_for_duplication (MIR_context_t ctx MIR_UNUSED, VARR (MIR_insn_t) * labels,
                                          VARR (MIR_insn_t) * branch_insns, MIR_insn_t insn,
                                          MIR_insn_t new_insn) {
  if (MIR_any_branch_code_p (insn->code) || insn->code == MIR_LADDR || insn->code == MIR_PRBEQ
      || insn->code == MIR_PRBNE) {
    VARR_PUSH (MIR_insn_t, branch_insns, new_insn);
  } else if (insn->code == MIR_LABEL) {
    mir_assert (insn->data == NULL);
    insn->data = new_insn;
    VARR_PUSH (MIR_insn_t, labels, insn);
  }
}

static void redirect_duplicated_labels (MIR_context_t ctx MIR_UNUSED, VARR (MIR_insn_t) * labels,
                                        VARR (MIR_insn_t) * branch_insns) {
  MIR_insn_t insn;

  while (VARR_LENGTH (MIR_insn_t, branch_insns) != 0) { /* redirect new label operands */
    size_t start_label_nop = 0, bound_label_nop = 1, n;

    insn = VARR_POP (MIR_insn_t, branch_insns);
    if (insn->code == MIR_JMPI) continue;
    if (insn->code == MIR_SWITCH) {
      start_label_nop = 1;
      bound_label_nop = start_label_nop + insn->nops - 1;
    } else if (insn->code == MIR_LADDR) {
      start_label_nop = 1;
      bound_label_nop = 2;
    }
    for (n = start_label_nop; n < bound_label_nop; n++)
      insn->ops[n].u.label = insn->ops[n].u.label->data;
  }
  while (VARR_LENGTH (MIR_insn_t, labels) != 0) { /* reset data */
    insn = VARR_POP (MIR_insn_t, labels);
    insn->data = NULL;
  }
}

void _MIR_duplicate_func_insns (MIR_context_t ctx, MIR_item_t func_item) {
  MIR_func_t func;
  MIR_insn_t insn, new_insn;
  VARR (MIR_insn_t) * labels, *branch_insns;

  mir_assert (func_item != NULL && func_item->item_type == MIR_func_item);
  func = func_item->u.func;
  mir_assert (DLIST_HEAD (MIR_insn_t, func->original_insns) == NULL);
  func->original_vars_num = VARR_LENGTH (MIR_var_t, func->vars);
  func->original_insns = func->insns;
  DLIST_INIT (MIR_insn_t, func->insns);
  VARR_CREATE (MIR_insn_t, labels, ctx->alloc, 0);
  VARR_CREATE (MIR_insn_t, branch_insns, ctx->alloc, 0);
  for (insn = DLIST_HEAD (MIR_insn_t, func->original_insns); insn != NULL;
       insn = DLIST_NEXT (MIR_insn_t, insn)) { /* copy insns and collect label info */
    new_insn = MIR_copy_insn (ctx, insn);
    DLIST_APPEND (MIR_insn_t, func->insns, new_insn);
    store_labels_for_duplication (ctx, labels, branch_insns, insn, new_insn);
  }
  for (MIR_lref_data_t lref = func->first_lref; lref != NULL; lref = lref->next) {
    lref->orig_label = lref->label;
    lref->orig_label2 = lref->label2;
    lref->label = lref->label->data;
    if (lref->label2 != NULL) lref->label2 = lref->label2->data;
  }
  redirect_duplicated_labels (ctx, labels, branch_insns);
  VARR_DESTROY (MIR_insn_t, labels);
  VARR_DESTROY (MIR_insn_t, branch_insns);
}

void _MIR_restore_func_insns (MIR_context_t ctx, MIR_item_t func_item) {
  MIR_func_t func;
  MIR_insn_t insn;

  mir_assert (func_item != NULL && func_item->item_type == MIR_func_item);
  func = func_item->u.func;
  while (VARR_LENGTH (MIR_var_t, func->vars) > func->original_vars_num) {
    reg_desc_t *rd;
    int res_p = TRUE;
    size_t rdn, tab_rdn;
    MIR_var_t var = VARR_POP (MIR_var_t, func->vars);
    func_regs_t func_regs = func->internal;

    rd = find_rd_by_name (ctx, var.name, func);
    mir_assert (rd != NULL);
    rdn = rd - VARR_ADDR (reg_desc_t, func_regs->reg_descs);
    res_p &= HTAB_DO (size_t, func_regs->name2rdn_tab, rdn, HTAB_DELETE, tab_rdn);
    res_p &= HTAB_DO (size_t, func_regs->reg2rdn_tab, rdn, HTAB_DELETE, tab_rdn);
    mir_assert (res_p);
  }
  while ((insn = DLIST_HEAD (MIR_insn_t, func->insns)) != NULL)
    MIR_remove_insn (ctx, func_item, insn);
  func->insns = func->original_insns;
  DLIST_INIT (MIR_insn_t, func->original_insns);
  for (MIR_lref_data_t lref = func->first_lref; lref != NULL; lref = lref->next) {
    lref->label = lref->orig_label;
    lref->label2 = lref->orig_label2;
    lref->orig_label = lref->orig_label2 = NULL;
  }
}

static void set_item_name (MIR_item_t item, const char *name) {
  mir_assert (item != NULL);
  switch (item->item_type) {
  case MIR_func_item: item->u.func->name = name; break;
  case MIR_proto_item: item->u.proto->name = name; break;
  case MIR_import_item: item->u.import_id = name; break;
  case MIR_export_item: item->u.export_id = name; break;
  case MIR_forward_item: item->u.forward_id = name; break;
  case MIR_bss_item: item->u.bss->name = name; break;
  case MIR_data_item: item->u.data->name = name; break;
  case MIR_ref_data_item: item->u.ref_data->name = name; break;
  case MIR_lref_data_item: item->u.lref_data->name = name; break;
  case MIR_expr_data_item: item->u.expr_data->name = name; break;
  default: mir_assert (FALSE);
  }
}

static void change_var_names (MIR_context_t new_ctx, VARR (MIR_var_t) * vars) {
  for (size_t i = 0; i < VARR_LENGTH (MIR_var_t, vars); i++) {
    MIR_var_t *var_ptr = &VARR_ADDR (MIR_var_t, vars)[i];
    var_ptr->name = get_ctx_str (new_ctx, var_ptr->name);
  }
}

/* It is not thread-safe */
void MIR_change_module_ctx (MIR_context_t old_ctx, MIR_module_t m, MIR_context_t new_ctx) {
  MIR_item_t item, tab_item;
  MIR_op_mode_t mode;
  const char *name, *new_name;

  DLIST_REMOVE (MIR_module_t, *MIR_get_module_list (old_ctx), m);
  DLIST_APPEND (MIR_module_t, *MIR_get_module_list (new_ctx), m);
  m->name = get_ctx_str (new_ctx, m->name);
  for (item = DLIST_HEAD (MIR_item_t, m->items); item != NULL;
       item = DLIST_NEXT (MIR_item_t, item)) {
    if (item->addr != NULL)
      MIR_get_error_func (old_ctx) (MIR_ctx_change_error, "Change context of a loaded module");
    if ((name = MIR_item_name (old_ctx, item)) != NULL) {
      new_name = get_ctx_str (new_ctx, name);
      if (item_tab_find (old_ctx, name, m) != item) {
        set_item_name (item, new_name);
      } else {
        item_tab_remove (old_ctx, item);
        set_item_name (item, new_name);
        tab_item = item_tab_insert (new_ctx, item);
        mir_assert (item == tab_item);
      }
    }
    if (item->item_type == MIR_proto_item) {
      change_var_names (new_ctx, item->u.proto->args);
    } else if (item->item_type == MIR_func_item) {
      func_regs_t func_regs = item->u.func->internal;
      reg_desc_t *rds = VARR_ADDR (reg_desc_t, func_regs->reg_descs);

      for (size_t i = 1; i < VARR_LENGTH (reg_desc_t, func_regs->reg_descs); i++) {
        rds[i].name = (char *) get_ctx_str (new_ctx, rds[i].name);
        if (rds[i].hard_reg_name != NULL)
          rds[i].hard_reg_name = (char *) get_ctx_str (new_ctx, rds[i].hard_reg_name);
      }
      change_var_names (new_ctx, item->u.func->vars);
      if (item->u.func->global_vars != NULL) change_var_names (new_ctx, item->u.func->global_vars);

      for (MIR_insn_t insn = DLIST_HEAD (MIR_insn_t, item->u.func->insns); insn != NULL;
           insn = DLIST_NEXT (MIR_insn_t, insn)) {
        for (size_t i = 0; i < insn->nops; i++) {
          if ((mode = insn->ops[i].mode) == MIR_OP_STR) {
            insn->ops[i].u.str = get_ctx_string (new_ctx, insn->ops[i].u.str).str;
          } else if (mode == MIR_OP_MEM) {
            if (insn->ops[i].u.mem.alias != 0)
              insn->ops[i].u.mem.alias
                = MIR_alias (new_ctx, MIR_alias_name (old_ctx, insn->ops[i].u.mem.alias));
            if (insn->ops[i].u.mem.nonalias != 0)
              insn->ops[i].u.mem.nonalias
                = MIR_alias (new_ctx, MIR_alias_name (old_ctx, insn->ops[i].u.mem.nonalias));
          }
        }
      }
    }
  }
#undef curr_label_num
  if (new_ctx->curr_label_num < old_ctx->curr_label_num)
    new_ctx->curr_label_num = old_ctx->curr_label_num;
#define curr_label_num ctx->curr_label_num
}

static void output_type (MIR_context_t ctx, FILE *f, MIR_type_t tp) {
  fprintf (f, "%s", MIR_type_str (ctx, tp));
}

static void output_disp (FILE *f, MIR_disp_t disp) { fprintf (f, "%" PRId64, (int64_t) disp); }

static void output_scale (FILE *f, unsigned scale) { fprintf (f, "%u", scale); }

static void output_reg (MIR_context_t ctx, FILE *f, MIR_func_t func, MIR_reg_t reg) {
  fprintf (f, "%s", MIR_reg_name (ctx, reg, func));
}

static void output_hard_reg (FILE *f, MIR_reg_t hreg) { fprintf (f, "hr%u", hreg); }

static int var_is_reg_p (MIR_reg_t var);
static MIR_reg_t var2reg (MIR_reg_t var);
static void output_var (MIR_context_t ctx, FILE *f, MIR_func_t func, MIR_reg_t var) {
  var_is_reg_p (var) ? output_reg (ctx, f, func, var2reg (var)) : output_hard_reg (f, var);
}

static void output_label (MIR_context_t ctx, FILE *f, MIR_func_t func, MIR_label_t label);

void MIR_output_str (MIR_context_t ctx MIR_UNUSED, FILE *f, MIR_str_t str) {
  fprintf (f, "\"");
  for (size_t i = 0; i < str.len; i++)
    if (str.s[i] == '\\')
      fprintf (f, "\\\\");
    else if (str.s[i] == '"')
      fprintf (f, "\\\"");
    else if (isprint (str.s[i]))
      fprintf (f, "%c", str.s[i]);
    else if (str.s[i] == '\n')
      fprintf (f, "\\n");
    else if (str.s[i] == '\t')
      fprintf (f, "\\t");
    else if (str.s[i] == '\v')
      fprintf (f, "\\v");
    else if (str.s[i] == '\a')
      fprintf (f, "\\a");
    else if (str.s[i] == '\b')
      fprintf (f, "\\b");
    else if (str.s[i] == '\f')
      fprintf (f, "\\f");
    else
      fprintf (f, "\\%03o", (unsigned char) str.s[i]);
  fprintf (f, "\"");
}

void MIR_output_op (MIR_context_t ctx, FILE *f, MIR_op_t op, MIR_func_t func) {
  switch (op.mode) {
  case MIR_OP_REG: output_reg (ctx, f, func, op.u.reg); break;
  case MIR_OP_VAR: output_var (ctx, f, func, op.u.var); break;
  case MIR_OP_INT: fprintf (f, "%" PRId64, op.u.i); break;
  case MIR_OP_UINT: fprintf (f, "%" PRIu64, op.u.u); break;
  case MIR_OP_FLOAT: fprintf (f, "%.*ef", FLT_MANT_DIG, op.u.f); break;
  case MIR_OP_DOUBLE: fprintf (f, "%.*e", DBL_MANT_DIG, op.u.d); break;
  case MIR_OP_LDOUBLE: fprintf (f, "%.*LeL", LDBL_MANT_DIG, op.u.ld); break;
  case MIR_OP_MEM:
  case MIR_OP_VAR_MEM: {
    MIR_reg_t no_reg = op.mode == MIR_OP_MEM ? 0 : MIR_NON_VAR;

    output_type (ctx, f, op.u.mem.type);
    fprintf (f, ":");
    if (op.u.mem.disp != 0 || (op.u.mem.base == no_reg && op.u.mem.index == no_reg))
      output_disp (f, op.u.mem.disp);
    if (op.u.mem.base != no_reg || op.u.mem.index != no_reg) {
      fprintf (f, "(");
      if (op.u.mem.base != no_reg)
        (op.mode == MIR_OP_MEM ? output_reg : output_var) (ctx, f, func, op.u.mem.base);
      if (op.u.mem.index != no_reg) {
        fprintf (f, ", ");
        (op.mode == MIR_OP_MEM ? output_reg : output_var) (ctx, f, func, op.u.mem.index);
        if (op.u.mem.scale != 1) {
          fprintf (f, ", ");
          output_scale (f, op.u.mem.scale);
        }
      }
      fprintf (f, ")");
    }
    if (op.u.mem.alias != 0 || op.u.mem.nonalias != 0) {
      fprintf (f, ":");
      if (op.u.mem.alias != 0) fprintf (f, "%s", MIR_alias_name (ctx, op.u.mem.alias));
      if (op.u.mem.nonalias != 0) fprintf (f, ":%s", MIR_alias_name (ctx, op.u.mem.nonalias));
    }
    break;
  }
  case MIR_OP_REF:
    if (op.u.ref->module != func->func_item->module) fprintf (f, "%s.", op.u.ref->module->name);
    fprintf (f, "%s", MIR_item_name (ctx, op.u.ref));
    break;
  case MIR_OP_STR: MIR_output_str (ctx, f, op.u.str); break;
  case MIR_OP_LABEL: output_label (ctx, f, func, op.u.label); break;
  default: mir_assert (FALSE);
  }
}

static void output_label (MIR_context_t ctx, FILE *f, MIR_func_t func, MIR_label_t label) {
  fprintf (f, "L");
  MIR_output_op (ctx, f, label->ops[0], func);
}

void MIR_output_insn (MIR_context_t ctx, FILE *f, MIR_insn_t insn, MIR_func_t func, int newline_p) {
  size_t i, nops;

  mir_assert (insn != NULL);
  if (insn->code == MIR_LABEL) {
    output_label (ctx, f, func, insn);
    if (newline_p) fprintf (f, ":\n");
    return;
  }
  fprintf (f, "\t%s", MIR_insn_name (ctx, insn->code));
  nops = MIR_insn_nops (ctx, insn);
  for (i = 0; i < nops; i++) {
    fprintf (f, i == 0 ? "\t" : ", ");
    MIR_output_op (ctx, f, insn->ops[i], func);
  }
  if (insn->code == MIR_UNSPEC)
    fprintf (f, " # %s", VARR_GET (MIR_proto_t, unspec_protos, insn->ops[0].u.u)->name);
  if (newline_p) fprintf (f, "\n");
}

static void output_func_proto (MIR_context_t ctx, FILE *f, size_t nres, MIR_type_t *types,
                               size_t nargs, VARR (MIR_var_t) * args, int vararg_p) {
  size_t i;
  MIR_var_t var;

  for (i = 0; i < nres; i++) {
    if (i != 0) fprintf (f, ", ");
    fprintf (f, "%s", MIR_type_str (ctx, types[i]));
  }
  for (i = 0; i < nargs; i++) {
    var = VARR_GET (MIR_var_t, args, i);
    if (i != 0 || nres != 0) fprintf (f, ", ");
    mir_assert (var.name != NULL);
    if (!MIR_all_blk_type_p (var.type))
      fprintf (f, "%s:%s", MIR_type_str (ctx, var.type), var.name);
    else
      fprintf (f, "%s:%lu(%s)", MIR_type_str (ctx, var.type), (unsigned long) var.size, var.name);
  }
  if (vararg_p) fprintf (f, nargs == 0 && nres == 0 ? "..." : ", ...");
  fprintf (f, "\n");
}

static void output_vars (MIR_context_t ctx, FILE *f, MIR_func_t func, VARR (MIR_var_t) * vars,
                         size_t start, size_t vars_num, const char *prefix) {
  if (vars == NULL || vars_num == 0) return;
  for (size_t i = 0; i < vars_num; i++) {
    MIR_var_t var = VARR_GET (MIR_var_t, vars, i + start);
    if (i % 8 == 0) {
      if (i != 0) fprintf (f, "\n");
      fprintf (f, "\t%s\t", prefix);
    }
    if (i % 8 != 0) fprintf (f, ", ");
    fprintf (f, "%s:%s", MIR_type_str (ctx, var.type), var.name);
    MIR_reg_t reg = MIR_reg (ctx, var.name, func);
    const char *hard_reg_name = MIR_reg_hard_reg_name (ctx, reg, func);
    if (hard_reg_name != NULL) fprintf (f, ":%s", hard_reg_name);
  }
  fprintf (f, "\n");
}

void _MIR_output_data_item_els (MIR_context_t ctx, FILE *f, MIR_item_t item, int c_p) {
  mir_assert (item->item_type == MIR_data_item);
  MIR_data_t data = item->u.data;
  for (size_t i = 0; i < data->nel; i++) {
    switch (data->el_type) {
    case MIR_T_I8: fprintf (f, "%" PRId8, ((int8_t *) data->u.els)[i]); break;
    case MIR_T_U8: fprintf (f, "%" PRIu8, ((uint8_t *) data->u.els)[i]); break;
    case MIR_T_I16: fprintf (f, "%" PRId16, ((int16_t *) data->u.els)[i]); break;
    case MIR_T_U16: fprintf (f, "%" PRIu16, ((uint16_t *) data->u.els)[i]); break;
    case MIR_T_I32: fprintf (f, "%" PRId32, ((int32_t *) data->u.els)[i]); break;
    case MIR_T_U32: fprintf (f, "%" PRIu32, ((uint32_t *) data->u.els)[i]); break;
    case MIR_T_I64: fprintf (f, "%" PRId64, ((int64_t *) data->u.els)[i]); break;
    case MIR_T_U64: fprintf (f, "%" PRIu64, ((uint64_t *) data->u.els)[i]); break;
    case MIR_T_F: fprintf (f, "%.*ef", FLT_MANT_DIG, ((float *) data->u.els)[i]); break;
    case MIR_T_D: fprintf (f, "%.*e", DBL_MANT_DIG, ((double *) data->u.els)[i]); break;
    case MIR_T_LD:
      fprintf (f, "%.*LeL", LDBL_MANT_DIG, ((long double *) data->u.els)[i]);
      break;
      /* only ptr as ref ??? */
    case MIR_T_P: fprintf (f, "0x%" PRIxPTR, ((uintptr_t *) data->u.els)[i]); break;
    default: mir_assert (FALSE);
    }
    if (i + 1 < data->nel) fprintf (f, ", ");
  }
  if (data->el_type == MIR_T_U8 && data->nel != 0 && data->u.els[data->nel - 1] == '\0') {
    fprintf (f, c_p ? "/* " : " # "); /* print possible string as a comment */
    MIR_output_str (ctx, f, (MIR_str_t){data->nel, (char *) data->u.els});
    if (c_p) fprintf (f, " */");
  }
}

void MIR_output_item (MIR_context_t ctx, FILE *f, MIR_item_t item) {
  MIR_insn_t insn;
  MIR_func_t func;
  MIR_proto_t proto;
  MIR_data_t data;
  MIR_ref_data_t ref_data;
  MIR_expr_data_t expr_data;
  size_t vars_num, nglobal;

  mir_assert (f != NULL && item != NULL);
  if (item->item_type == MIR_export_item) {
    fprintf (f, "\texport\t%s\n", item->u.export_id);
    return;
  }
  if (item->item_type == MIR_import_item) {
    fprintf (f, "\timport\t%s\n", item->u.import_id);
    return;
  }
  if (item->item_type == MIR_forward_item) {
    fprintf (f, "\tforward\t%s\n", item->u.forward_id);
    return;
  }
  if (item->item_type == MIR_bss_item) {
    if (item->u.bss->name != NULL) fprintf (f, "%s:", item->u.bss->name);
    fprintf (f, "\tbss\t%" PRIu64 "\n", item->u.bss->len);
    return;
  }
  if (item->item_type == MIR_ref_data_item) {
    ref_data = item->u.ref_data;
    if (ref_data->name != NULL) fprintf (f, "%s:", ref_data->name);
    fprintf (f, "\tref\t%s, %" PRId64 "\n", MIR_item_name (ctx, ref_data->ref_item),
             (int64_t) ref_data->disp);
    return;
  }
  if (item->item_type == MIR_lref_data_item) {
    MIR_lref_data_t lref_data = item->u.lref_data;
    if (lref_data->name != NULL) fprintf (f, "%s:", lref_data->name);
    mir_assert (lref_data->label->ops[0].mode == MIR_OP_INT);
    fprintf (f, "\tlref\tL%" PRId64, lref_data->label->ops[0].u.i);
    mir_assert (lref_data->label2 == NULL || lref_data->label2->ops[0].mode == MIR_OP_INT);
    if (lref_data->label2 != NULL) fprintf (f, ", L%" PRId64, lref_data->label2->ops[0].u.i);
    if (lref_data->disp != 0) fprintf (f, ", %" PRId64, (int64_t) lref_data->disp);
    fprintf (f, "\n");
    return;
  }
  if (item->item_type == MIR_expr_data_item) {
    expr_data = item->u.expr_data;
    if (expr_data->name != NULL) fprintf (f, "%s:", expr_data->name);
    fprintf (f, "\texpr\t%s", MIR_item_name (ctx, expr_data->expr_item));
  }
  if (item->item_type == MIR_data_item) {
    data = item->u.data;
    if (data->name != NULL) fprintf (f, "%s:", data->name);
    fprintf (f, "\t%s\t", MIR_type_str (ctx, data->el_type));
    _MIR_output_data_item_els (ctx, f, item, FALSE);
    fprintf (f, "\n");
    return;
  }
  if (item->item_type == MIR_proto_item) {
    proto = item->u.proto;
    fprintf (f, "%s:\tproto\t", proto->name);
    output_func_proto (ctx, f, proto->nres, proto->res_types, VARR_LENGTH (MIR_var_t, proto->args),
                       proto->args, proto->vararg_p);
    return;
  }
  func = item->u.func;
  fprintf (f, "%s:\tfunc\t", func->name);
  output_func_proto (ctx, f, func->nres, func->res_types, func->nargs, func->vars, func->vararg_p);
  vars_num = VARR_LENGTH (MIR_var_t, func->vars) - func->nargs;
  nglobal = func->global_vars == NULL ? 0 : VARR_LENGTH (MIR_var_t, func->global_vars);
  output_vars (ctx, f, func, func->vars, func->nargs, vars_num, "local");
  output_vars (ctx, f, func, func->global_vars, 0, nglobal, "global");
  fprintf (f, "\n# %u arg%s, %ld local%s, %ld global%s\n", func->nargs, func->nargs == 1 ? "" : "s",
           (long) vars_num, vars_num == 1 ? "" : "s", (long) nglobal, nglobal == 1 ? "" : "s");
  for (insn = DLIST_HEAD (MIR_insn_t, func->insns); insn != NULL;
       insn = DLIST_NEXT (MIR_insn_t, insn))
    MIR_output_insn (ctx, f, insn, func, TRUE);
  fprintf (f, "\tendfunc\n");
}

void MIR_output_module (MIR_context_t ctx, FILE *f, MIR_module_t module) {
  mir_assert (f != NULL && module != NULL);
  fprintf (f, "%s:\tmodule\n", module->name);
  for (MIR_item_t item = DLIST_HEAD (MIR_item_t, module->items); item != NULL;
       item = DLIST_NEXT (MIR_item_t, item))
    MIR_output_item (ctx, f, item);
  fprintf (f, "\tendmodule\n");
}

void MIR_output (MIR_context_t ctx, FILE *f) {
  mir_assert (f != NULL);
  for (MIR_module_t module = DLIST_HEAD (MIR_module_t, all_modules); module != NULL;
       module = DLIST_NEXT (MIR_module_t, module))
    MIR_output_module (ctx, f, module);
}

/* New Page */
/* This page contains code for simplification and inlining */

static MIR_insn_t insert_op_insn (MIR_context_t ctx, int out_p, MIR_item_t func_item,
                                  MIR_insn_t anchor, MIR_insn_t insn) {
  if (!out_p) {
    MIR_insert_insn_before (ctx, func_item, anchor, insn);
    return anchor;
  }
  MIR_insert_insn_after (ctx, func_item, anchor, insn);
  return insn;
}

typedef struct {
  MIR_insn_code_t code;
  MIR_type_t type;
  MIR_op_t op1, op2;
  MIR_reg_t reg;
} val_t;

DEF_HTAB (val_t);

struct simplify_ctx {
  HTAB (val_t) * val_tab;
  /* temp_insns is for branch or ret insns */
  VARR (MIR_insn_t) * temp_insns, *cold_insns, *labels;
  VARR (MIR_reg_t) * inline_reg_map;
  VARR (MIR_insn_t) * anchors;
  VARR (size_t) * alloca_sizes;
  size_t new_label_num, inlined_calls, inline_insns_before, inline_insns_after;
};

#define val_tab ctx->simplify_ctx->val_tab
#define temp_insns ctx->simplify_ctx->temp_insns
#define cold_insns ctx->simplify_ctx->cold_insns
#define labels ctx->simplify_ctx->labels
#define inline_reg_map ctx->simplify_ctx->inline_reg_map
#define anchors ctx->simplify_ctx->anchors
#define alloca_sizes ctx->simplify_ctx->alloca_sizes
#define new_label_num ctx->simplify_ctx->new_label_num
#define inlined_calls ctx->simplify_ctx->inlined_calls
#define inline_insns_before ctx->simplify_ctx->inline_insns_before
#define inline_insns_after ctx->simplify_ctx->inline_insns_after

static htab_hash_t val_hash (val_t v, void *arg) {
  MIR_context_t ctx = arg;
  htab_hash_t h;

  h = (htab_hash_t) mir_hash_step (mir_hash_init (0), (uint64_t) v.code);
  h = (htab_hash_t) mir_hash_step (h, (uint64_t) v.type);
  h = MIR_op_hash_step (ctx, h, v.op1);
  if (v.code != MIR_INSN_BOUND) h = MIR_op_hash_step (ctx, h, v.op2);
  return (htab_hash_t) mir_hash_finish (h);
}

static int val_eq (val_t v1, val_t v2, void *arg) {
  MIR_context_t ctx = arg;

  if (v1.code != v2.code || v1.type != v2.type || !MIR_op_eq_p (ctx, v1.op1, v2.op1)) return FALSE;
  return v1.code == MIR_INSN_BOUND || MIR_op_eq_p (ctx, v1.op2, v2.op2);
}

static void simplify_init (MIR_context_t ctx) {
  if ((ctx->simplify_ctx = MIR_malloc (ctx->alloc, sizeof (struct simplify_ctx))) == NULL)
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for ctx");
  HTAB_CREATE (val_t, val_tab, ctx->alloc, 512, val_hash, val_eq, ctx);
  VARR_CREATE (MIR_insn_t, temp_insns, ctx->alloc, 0);
  VARR_CREATE (MIR_insn_t, cold_insns, ctx->alloc, 0);
  VARR_CREATE (MIR_insn_t, labels, ctx->alloc, 0);
  VARR_CREATE (MIR_reg_t, inline_reg_map, ctx->alloc, 256);
  VARR_CREATE (MIR_insn_t, anchors, ctx->alloc, 32);
  VARR_CREATE (size_t, alloca_sizes, ctx->alloc, 32);
  inlined_calls = inline_insns_before = inline_insns_after = 0;
}

static void simplify_finish (MIR_context_t ctx) {
  VARR_DESTROY (size_t, alloca_sizes);
  VARR_DESTROY (MIR_insn_t, anchors);
  VARR_DESTROY (MIR_reg_t, inline_reg_map);
#if 0
  if (inlined_calls != 0)
    fprintf (stderr, "inlined calls = %lu, insns before = %lu, insns_after = %lu, ratio = %.2f\n",
             inlined_calls, inline_insns_before, inline_insns_after,
             (double) inline_insns_after / inline_insns_before);
#endif
  VARR_DESTROY (MIR_insn_t, labels);
  VARR_DESTROY (MIR_insn_t, temp_insns);
  VARR_DESTROY (MIR_insn_t, cold_insns);
  HTAB_DESTROY (val_t, val_tab);
  MIR_free (ctx->alloc, ctx->simplify_ctx);
  ctx->simplify_ctx = NULL;
}

static void simplify_module_init (MIR_context_t ctx) {
  new_label_num = 0;
  VARR_TRUNC (uint8_t, used_label_p, 0);
}

static void vn_empty (MIR_context_t ctx) { HTAB_CLEAR (val_t, val_tab); }

static MIR_reg_t vn_add_val (MIR_context_t ctx, MIR_func_t func, MIR_type_t type,
                             MIR_insn_code_t code, MIR_op_t op1, MIR_op_t op2) {
  val_t val, tab_val;

  val.type = type;
  val.code = code;
  val.op1 = op1;
  val.op2 = op2;
  if (HTAB_DO (val_t, val_tab, val, HTAB_FIND, tab_val)) return tab_val.reg;
  val.reg = new_temp_reg (ctx, type, func);
  HTAB_DO (val_t, val_tab, val, HTAB_INSERT, tab_val);
  return val.reg;
}

void _MIR_get_temp_item_name (MIR_context_t ctx MIR_UNUSED, MIR_module_t module, char *buff,
                              size_t buff_len) {
  mir_assert (module != NULL);
  module->last_temp_item_num++;
  snprintf (buff, buff_len, "%s%u", TEMP_ITEM_NAME_PREFIX, (unsigned) module->last_temp_item_num);
}

static MIR_insn_code_t get_type_move_code (MIR_type_t type) {
  return (type == MIR_T_F    ? MIR_FMOV
          : type == MIR_T_D  ? MIR_DMOV
          : type == MIR_T_LD ? MIR_LDMOV
                             : MIR_MOV);
}

static void simplify_op (MIR_context_t ctx, MIR_item_t func_item, MIR_insn_t insn, int nop,
                         int out_p, MIR_insn_code_t code, int keep_ref_p, int mem_float_p) {
  mir_assert (insn != NULL && func_item != NULL);
  MIR_op_t new_op, mem_op, *op = &insn->ops[nop];
  MIR_insn_t new_insn;
  MIR_func_t func = func_item->u.func;
  MIR_type_t type;
  MIR_op_mode_t value_mode = op->value_mode;
  int move_p = code == MIR_MOV || code == MIR_FMOV || code == MIR_DMOV || code == MIR_LDMOV;

  if (code == MIR_PHI || code == MIR_USE) return; /* do nothing: it is use or phi insn */
  if (code == MIR_UNSPEC && nop == 0) return;     /* do nothing: it is an unspec code */
  if (MIR_call_code_p (code)) {
    if (nop == 0) return; /* do nothing: it is a prototype */
    if (nop == 1 && op->mode == MIR_OP_REF
        && (op->u.ref->item_type == MIR_import_item || op->u.ref->item_type == MIR_func_item))
      return; /* do nothing: it is an immediate operand */
  }
  if (code == MIR_VA_ARG && nop == 2) return; /* do nothing: this operand is used as a type */
  if ((code == MIR_PRBEQ || code == MIR_PRBNE) && nop == 2) return; /* it is a property */
  if (code == MIR_PRSET && nop == 1) return;                        /* it is a property */
  switch (op->mode) {
  case MIR_OP_REF:
    if (keep_ref_p) break;
    /* falls through */
  case MIR_OP_INT:
  case MIR_OP_UINT:
  case MIR_OP_FLOAT:
  case MIR_OP_DOUBLE:
  case MIR_OP_LDOUBLE:
  case MIR_OP_STR:
    mir_assert (!out_p);
    if (op->mode == MIR_OP_REF) {
      for (MIR_item_t item = op->u.ref; item != NULL; item = item->ref_def)
        if (item->item_type != MIR_export_item && item->item_type != MIR_forward_item) {
          op->u.ref = item;
          break;
        }
    } else if (op->mode == MIR_OP_STR
               || (mem_float_p
                   && (op->mode == MIR_OP_FLOAT || op->mode == MIR_OP_DOUBLE
                       || op->mode == MIR_OP_LDOUBLE))) {
      const char *name;
      char buff[50];
      MIR_item_t item;
      MIR_module_t m = curr_module;

      curr_module = func_item->module;
      _MIR_get_temp_item_name (ctx, curr_module, buff, sizeof (buff));
      name = buff;
      if (op->mode == MIR_OP_STR) {
        item = MIR_new_string_data (ctx, name, op->u.str);
        *op = MIR_new_ref_op (ctx, item);
      } else {
        if (op->mode == MIR_OP_FLOAT)
          item = MIR_new_data (ctx, name, MIR_T_F, 1, (uint8_t *) &op->u.f);
        else if (op->mode == MIR_OP_DOUBLE)
          item = MIR_new_data (ctx, name, MIR_T_D, 1, (uint8_t *) &op->u.d);
        else
          item = MIR_new_data (ctx, name, MIR_T_LD, 1, (uint8_t *) &op->u.ld);
        type = op->mode == MIR_OP_FLOAT ? MIR_T_F : op->mode == MIR_OP_DOUBLE ? MIR_T_D : MIR_T_LD;
        *op = MIR_new_ref_op (ctx, item);
        new_op = MIR_new_reg_op (ctx, vn_add_val (ctx, func, MIR_T_I64, MIR_INSN_BOUND, *op, *op));
        MIR_insert_insn_before (ctx, func_item, insn, MIR_new_insn (ctx, MIR_MOV, new_op, *op));
        *op = MIR_new_mem_op (ctx, type, 0, new_op.u.reg, 0, 1);
      }
      if (func_item->addr != NULL) /* The function was already loaded: we should load new data */
        load_bss_data_section (ctx, item, TRUE);
      curr_module = m;
    }
    if (move_p) return;
    type = (op->mode == MIR_OP_FLOAT     ? MIR_T_F
            : op->mode == MIR_OP_DOUBLE  ? MIR_T_D
            : op->mode == MIR_OP_LDOUBLE ? MIR_T_LD
            : op->mode == MIR_OP_MEM     ? op->u.mem.type
                                         : MIR_T_I64);
    new_op = MIR_new_reg_op (ctx, vn_add_val (ctx, func, type, MIR_INSN_BOUND, *op, *op));
    MIR_insert_insn_before (ctx, func_item, insn,
                            MIR_new_insn (ctx, get_type_move_code (type), new_op, *op));
    *op = new_op;
    break;
  case MIR_OP_REG:
    if (MIR_reg_hard_reg_name (ctx, op->u.reg, func) == NULL) break;
    int another_nop = nop == 0 ? 1 : 0;
    if (move_p && insn->ops[another_nop].mode == MIR_OP_REG
        && MIR_reg_hard_reg_name (ctx, insn->ops[another_nop].u.reg, func) == NULL)
      break;
    type = MIR_reg_type (ctx, op->u.reg, func);
    new_op = MIR_new_reg_op (ctx, vn_add_val (ctx, func, type, MIR_INSN_BOUND, *op, *op));
    if (out_p) {
      MIR_insert_insn_after (ctx, func_item, insn,
                             MIR_new_insn (ctx, get_type_move_code (type), *op, new_op));
    } else {
      MIR_insert_insn_before (ctx, func_item, insn,
                              MIR_new_insn (ctx, get_type_move_code (type), new_op, *op));
    }
    *op = new_op;
    break;
  case MIR_OP_VAR:
  case MIR_OP_LABEL: break; /* Do nothing */
  case MIR_OP_MEM: {
    MIR_op_t reg_op;
    MIR_reg_t addr_reg = 0;

    if (op->u.mem.base != 0 && MIR_reg_hard_reg_name (ctx, op->u.mem.base, func) != NULL) {
      reg_op = MIR_new_reg_op (ctx, op->u.mem.base);
      new_op
        = MIR_new_reg_op (ctx, vn_add_val (ctx, func, MIR_T_I64, MIR_INSN_BOUND, reg_op, reg_op));
      MIR_insert_insn_before (ctx, func_item, insn, MIR_new_insn (ctx, MIR_MOV, new_op, reg_op));
      op->u.mem.base = new_op.u.reg;
    }
    if (op->u.mem.index != 0 && MIR_reg_hard_reg_name (ctx, op->u.mem.index, func) != NULL) {
      reg_op = MIR_new_reg_op (ctx, op->u.mem.index);
      new_op
        = MIR_new_reg_op (ctx, vn_add_val (ctx, func, MIR_T_I64, MIR_INSN_BOUND, reg_op, reg_op));
      MIR_insert_insn_before (ctx, func_item, insn, MIR_new_insn (ctx, MIR_MOV, new_op, reg_op));
      op->u.mem.index = new_op.u.reg;
    }
    mem_op = *op;
    type = mem_op.u.mem.type;
    if (op->u.mem.base != 0 && op->u.mem.disp == 0
        && (op->u.mem.index == 0 || op->u.mem.scale == 0)) {
      addr_reg = op->u.mem.base;
    } else if (op->u.mem.base == 0 && op->u.mem.index != 0 && op->u.mem.scale == 1
               && op->u.mem.disp == 0) {
      addr_reg = op->u.mem.index;
    } else {
      int after_p = !move_p && out_p;
      MIR_reg_t disp_reg = 0, scale_ind_reg = op->u.mem.index;
      MIR_reg_t base_reg = op->u.mem.base, base_ind_reg = 0;

      if (op->u.mem.disp != 0) {
        MIR_op_t disp_op = MIR_new_int_op (ctx, op->u.mem.disp);

        disp_reg = vn_add_val (ctx, func, MIR_T_I64, MIR_INSN_BOUND, disp_op, disp_op);
        insn
          = insert_op_insn (ctx, after_p, func_item, insn,
                            MIR_new_insn (ctx, MIR_MOV, MIR_new_reg_op (ctx, disp_reg), disp_op));
      }
      if (scale_ind_reg != 0 && op->u.mem.scale > 1) {
        MIR_op_t ind_op = MIR_new_reg_op (ctx, op->u.mem.index);
        MIR_op_t scale_reg_op, scale_int_op = MIR_new_int_op (ctx, op->u.mem.scale);

        scale_reg_op = MIR_new_reg_op (ctx, vn_add_val (ctx, func, MIR_T_I64, MIR_INSN_BOUND,
                                                        scale_int_op, scale_int_op));
        insn = insert_op_insn (ctx, after_p, func_item, insn,
                               MIR_new_insn (ctx, MIR_MOV, scale_reg_op, scale_int_op));
        scale_ind_reg = vn_add_val (ctx, func, MIR_T_I64, MIR_MUL, ind_op, scale_reg_op);
        insn = insert_op_insn (ctx, after_p, func_item, insn,
                               MIR_new_insn (ctx, MIR_MUL, MIR_new_reg_op (ctx, scale_ind_reg),
                                             ind_op, scale_reg_op));
      }
      if (base_reg != 0 && scale_ind_reg != 0) {
        MIR_op_t base_op = MIR_new_reg_op (ctx, base_reg),
                 ind_op = MIR_new_reg_op (ctx, scale_ind_reg);

        base_ind_reg = vn_add_val (ctx, func, MIR_T_I64, MIR_ADD, base_op, ind_op);
        insn = insert_op_insn (ctx, after_p, func_item, insn,
                               MIR_new_insn (ctx, MIR_ADD, MIR_new_reg_op (ctx, base_ind_reg),
                                             base_op, ind_op));
      } else {
        base_ind_reg = base_reg != 0 ? base_reg : scale_ind_reg;
      }
      if (base_ind_reg == 0) {
        mir_assert (disp_reg != 0);
        addr_reg = disp_reg;
      } else if (disp_reg == 0) {
        mir_assert (base_ind_reg != 0);
        addr_reg = base_ind_reg;
      } else {
        MIR_op_t base_ind_op = MIR_new_reg_op (ctx, base_ind_reg);
        MIR_op_t disp_op = MIR_new_reg_op (ctx, disp_reg);

        addr_reg = vn_add_val (ctx, func, MIR_T_I64, MIR_ADD, base_ind_op, disp_op);
        insn = insert_op_insn (ctx, after_p, func_item, insn,
                               MIR_new_insn (ctx, MIR_ADD, MIR_new_reg_op (ctx, addr_reg),
                                             base_ind_op, disp_op));
      }
    }
    mem_op.u.mem.base = addr_reg;
    mem_op.u.mem.disp = 0;
    mem_op.u.mem.index = 0;
    mem_op.u.mem.scale = 0;
    if (move_p && (nop == 1 || insn->ops[1].mode == MIR_OP_REG)) {
      *op = mem_op;
    } else if (((code == MIR_VA_START && nop == 0)
                || ((code == MIR_VA_ARG || code == MIR_VA_BLOCK_ARG) && nop == 1)
                || (code == MIR_VA_END && nop == 0))
               && mem_op.u.mem.type == MIR_T_UNDEF) {
      *op = MIR_new_reg_op (ctx, addr_reg);
    } else if (!MIR_all_blk_type_p (mem_op.u.mem.type) || !MIR_call_code_p (code)) {
      type = (mem_op.u.mem.type == MIR_T_F || mem_op.u.mem.type == MIR_T_D
                  || mem_op.u.mem.type == MIR_T_LD
                ? mem_op.u.mem.type
                : MIR_T_I64);
      code = get_type_move_code (type);
      new_op = MIR_new_reg_op (ctx, vn_add_val (ctx, func, type, MIR_INSN_BOUND, mem_op, mem_op));
      if (out_p)
        new_insn = MIR_new_insn (ctx, code, mem_op, new_op);
      else
        new_insn = MIR_new_insn (ctx, code, new_op, mem_op);
      insn = insert_op_insn (ctx, out_p, func_item, insn, new_insn);
      *op = new_op;
    }
    break;
  }
  default:
    /* We don't simplify code with hard regs.  */
    mir_assert (FALSE);
  }
  op->value_mode = value_mode;
}

static void simplify_insn (MIR_context_t ctx, MIR_item_t func_item, MIR_insn_t insn, int keep_ref_p,
                           int mem_float_p) {
  int out_p;
  mir_assert (insn != NULL);
  MIR_insn_code_t code = insn->code;
  size_t i, nops = MIR_insn_nops (ctx, insn);

  for (i = 0; i < nops; i++) {
    MIR_insn_op_mode (ctx, insn, i, &out_p);
    simplify_op (ctx, func_item, insn, (int) i, out_p, code,
                 MIR_call_code_p (insn->code) && i == 1 && keep_ref_p, mem_float_p);
  }
}

static void make_one_ret (MIR_context_t ctx, MIR_item_t func_item) {
  size_t i, j;
  MIR_insn_code_t mov_code, ext_code;
  MIR_reg_t ret_reg;
  MIR_op_t reg_op, ret_reg_op;
  MIR_func_t func = func_item->u.func;
  MIR_type_t *res_types = func->res_types;
  MIR_insn_t ret_label = NULL, insn, last_ret_insn;
  VARR (MIR_insn_t) *ret_insns = temp_insns;
  VARR (MIR_op_t) *ret_ops = temp_ops;

  if (VARR_LENGTH (MIR_insn_t, ret_insns) == 0) return; /* jcall/jret func */
  last_ret_insn = VARR_LAST (MIR_insn_t, ret_insns);
  VARR_TRUNC (MIR_op_t, ret_ops, 0);
  if (VARR_LENGTH (MIR_insn_t, ret_insns) > 1) {
    ret_label = MIR_new_label (ctx);
    MIR_insert_insn_before (ctx, func_item, last_ret_insn, ret_label);
  }
  for (i = 0; i < func->nres; i++) { /* generate ext insn before last ret */
    ret_reg_op = last_ret_insn->ops[i];
    VARR_PUSH (MIR_op_t, ret_ops, ret_reg_op);
    switch (res_types[i]) {
    case MIR_T_I8: ext_code = MIR_EXT8; break;
    case MIR_T_U8: ext_code = MIR_UEXT8; break;
    case MIR_T_I16: ext_code = MIR_EXT16; break;
    case MIR_T_U16: ext_code = MIR_UEXT16; break;
    case MIR_T_I32: ext_code = MIR_EXT32; break;
    case MIR_T_U32: ext_code = MIR_UEXT32; break;
    default: ext_code = MIR_INVALID_INSN; break;
    }
    if (ext_code == MIR_INVALID_INSN) continue;
    mov_code = get_type_move_code (res_types[i]);
    ret_reg = _MIR_new_temp_reg (ctx, mov_code == MIR_MOV ? MIR_T_I64 : res_types[i], func);
    ret_reg_op = MIR_new_reg_op (ctx, ret_reg);
    MIR_insert_insn_before (ctx, func_item, last_ret_insn,
                            MIR_new_insn (ctx, ext_code, ret_reg_op, last_ret_insn->ops[i]));
    last_ret_insn->ops[i] = ret_reg_op;
  }
  /* change ret insns (except last one) to moves & jumps */
  for (i = 0; i < VARR_LENGTH (MIR_insn_t, ret_insns); i++) {
    insn = VARR_GET (MIR_insn_t, ret_insns, i);
    if (insn == last_ret_insn) continue;
    mir_assert (insn->code == MIR_RET || func->nres == MIR_insn_nops (ctx, insn));
    for (j = 0; j < func->nres; j++) {
      mov_code = get_type_move_code (res_types[j]);
      reg_op = insn->ops[j];
      mir_assert (reg_op.mode == MIR_OP_REG);
      ret_reg_op = VARR_GET (MIR_op_t, ret_ops, j);
      MIR_insert_insn_before (ctx, func_item, insn,
                              MIR_new_insn (ctx, mov_code, ret_reg_op, reg_op));
    }
    MIR_insert_insn_before (ctx, func_item, insn,
                            MIR_new_insn (ctx, MIR_JMP, MIR_new_label_op (ctx, ret_label)));
    MIR_remove_insn (ctx, func_item, insn);
  }
}

static void mark_used_label (MIR_context_t ctx, MIR_label_t label) {
  int64_t label_num = label->ops[0].u.i;
  while (label_num >= (int64_t) VARR_LENGTH (uint8_t, used_label_p))
    VARR_PUSH (uint8_t, used_label_p, FALSE);
  VARR_SET (uint8_t, used_label_p, label_num, TRUE);
}

static void remove_unused_and_enumerate_labels (MIR_context_t ctx, MIR_item_t func_item) {
  for (size_t i = 0; i < VARR_LENGTH (MIR_insn_t, labels); i++) {
    MIR_insn_t label = VARR_GET (MIR_insn_t, labels, i);
    int64_t label_num = label->ops[0].u.i;

    if (label_num < (int64_t) VARR_LENGTH (uint8_t, used_label_p)
        && VARR_GET (uint8_t, used_label_p, label_num)) {
      label->ops[0] = MIR_new_int_op (ctx, new_label_num++);
      continue;
    }
    MIR_remove_insn (ctx, func_item, label);
  }
  VARR_TRUNC (MIR_insn_t, labels, 0);
}

MIR_insn_code_t MIR_reverse_branch_code (MIR_insn_code_t code) {
  switch (code) {
  case MIR_BT: return MIR_BF;
  case MIR_BTS: return MIR_BFS;
  case MIR_BF: return MIR_BT;
  case MIR_BFS: return MIR_BTS;
  case MIR_BEQ: return MIR_BNE;
  case MIR_BEQS: return MIR_BNES;
  case MIR_BNE: return MIR_BEQ;
  case MIR_BNES: return MIR_BEQS;
  case MIR_BLT: return MIR_BGE;
  case MIR_BLTS: return MIR_BGES;
  case MIR_UBLT: return MIR_UBGE;
  case MIR_UBLTS: return MIR_UBGES;
  case MIR_BLE: return MIR_BGT;
  case MIR_BLES: return MIR_BGTS;
  case MIR_UBLE: return MIR_UBGT;
  case MIR_UBLES: return MIR_UBGTS;
  case MIR_BGT: return MIR_BLE;
  case MIR_BGTS: return MIR_BLES;
  case MIR_UBGT: return MIR_UBLE;
  case MIR_UBGTS: return MIR_UBLES;
  case MIR_BGE: return MIR_BLT;
  case MIR_BGES: return MIR_BLTS;
  case MIR_UBGE: return MIR_UBLT;
  case MIR_UBGES: return MIR_UBLTS;
  case MIR_BO: return MIR_BNO;
  case MIR_UBO: return MIR_UBNO;
  case MIR_BNO: return MIR_BO;
  case MIR_UBNO: return MIR_UBO;
  case MIR_PRBEQ: return MIR_PRBNE;
  case MIR_PRBNE: return MIR_PRBEQ;
  default: return MIR_INSN_BOUND;
  }
}

static MIR_insn_t skip_labels (MIR_label_t label, MIR_label_t stop) {
  for (MIR_insn_t insn = label;; insn = DLIST_NEXT (MIR_insn_t, insn))
    if (insn == NULL || insn->code != MIR_LABEL || insn == stop) return insn;
}

static MIR_insn_t last_label (MIR_label_t label) {
  MIR_insn_t next_insn;
  mir_assert (label->code == MIR_LABEL);
  while ((next_insn = DLIST_NEXT (MIR_insn_t, label)) != NULL && next_insn->code == MIR_LABEL)
    label = next_insn;
  return label;
}

static int64_t natural_alignment (int64_t s) { return s <= 2 ? s : s <= 4 ? 4 : s <= 8 ? 8 : 16; }

static const int MAX_JUMP_CHAIN_LEN = 32;

static int64_t get_alloca_size_align (int64_t size, int64_t *align_ptr) {
  int64_t align;
  size = size <= 0 ? 1 : size;
  *align_ptr = align = natural_alignment (size);
  return (size + align - 1) / align * align;
}

static int simplify_func (MIR_context_t ctx, MIR_item_t func_item, int mem_float_p) {
  MIR_func_t func = func_item->u.func;
  MIR_insn_t insn, next_insn, next_next_insn, jmp_insn, new_insn, label;
  MIR_insn_code_t ext_code, rev_code;
  int jmps_num = 0, inline_p = FALSE;

  if (func_item->item_type != MIR_func_item)
    MIR_get_error_func (ctx) (MIR_wrong_param_value_error, "MIR_remove_simplify: wrong func item");
  vn_empty (ctx);
  func = func_item->u.func;
  for (size_t i = 0; i < func->nargs; i++) {
    MIR_var_t var = VARR_GET (MIR_var_t, func->vars, i);

    if (var.type == MIR_T_I64 || var.type == MIR_T_U64 || var.type == MIR_T_F || var.type == MIR_T_D
        || var.type == MIR_T_LD)
      continue;
    switch (var.type) {
    case MIR_T_I8: ext_code = MIR_EXT8; break;
    case MIR_T_U8: ext_code = MIR_UEXT8; break;
    case MIR_T_I16: ext_code = MIR_EXT16; break;
    case MIR_T_U16: ext_code = MIR_UEXT16; break;
    case MIR_T_I32: ext_code = MIR_EXT32; break;
    case MIR_T_U32: ext_code = MIR_UEXT32; break;
    default: ext_code = MIR_INVALID_INSN; break;
    }
    if (ext_code != MIR_INVALID_INSN) {
      MIR_reg_t reg = MIR_reg (ctx, var.name, func);
      new_insn = MIR_new_insn (ctx, ext_code, MIR_new_reg_op (ctx, reg), MIR_new_reg_op (ctx, reg));
      MIR_prepend_insn (ctx, func_item, new_insn);
    }
  }
  VARR_TRUNC (MIR_insn_t, temp_insns, 0);
  VARR_TRUNC (MIR_insn_t, labels, 0);
  for (insn = DLIST_HEAD (MIR_insn_t, func->insns); insn != NULL; insn = next_insn) {
    MIR_insn_code_t code = insn->code;
    MIR_op_t temp_op;

    if ((code == MIR_MOV || code == MIR_FMOV || code == MIR_DMOV || code == MIR_LDMOV)
        && insn->ops[0].mode == MIR_OP_MEM && insn->ops[1].mode == MIR_OP_MEM) {
      temp_op = MIR_new_reg_op (ctx, new_temp_reg (ctx,
                                                   code == MIR_MOV    ? MIR_T_I64
                                                   : code == MIR_FMOV ? MIR_T_F
                                                   : code == MIR_DMOV ? MIR_T_D
                                                                      : MIR_T_LD,
                                                   func));
      MIR_insert_insn_after (ctx, func_item, insn, MIR_new_insn (ctx, code, insn->ops[0], temp_op));
      insn->ops[0] = temp_op;
    }
    if (code == MIR_RET) VARR_PUSH (MIR_insn_t, temp_insns, insn);
    if (code == MIR_LABEL) VARR_PUSH (MIR_insn_t, labels, insn);
    next_insn = DLIST_NEXT (MIR_insn_t, insn);
    if (code == MIR_ALLOCA
        && (insn->ops[1].mode == MIR_OP_INT || insn->ops[1].mode == MIR_OP_UINT)) {
      /* Consolidate adjacent allocas */
      int64_t size, overall_size, align, max_align;

      overall_size = get_alloca_size_align (insn->ops[1].u.i, &max_align);
      while (next_insn != NULL && next_insn->code == MIR_ALLOCA
             && (next_insn->ops[1].mode == MIR_OP_INT || next_insn->ops[1].mode == MIR_OP_UINT)
             && !MIR_op_eq_p (ctx, insn->ops[0], next_insn->ops[0])) {
        size = get_alloca_size_align (next_insn->ops[1].u.i, &align);
        if (max_align < align) {
          max_align = align;
          overall_size = (overall_size + align - 1) / align * align;
        }
        new_insn = MIR_new_insn (ctx, MIR_PTR32 ? MIR_ADDS : MIR_ADD, next_insn->ops[0],
                                 insn->ops[0], MIR_new_int_op (ctx, overall_size));
        overall_size += size;
        MIR_insert_insn_before (ctx, func_item, next_insn, new_insn);
        MIR_remove_insn (ctx, func_item, next_insn);
        next_insn = DLIST_NEXT (MIR_insn_t, new_insn);
      }
      insn->ops[1].u.i = overall_size;
      next_insn = DLIST_NEXT (MIR_insn_t, insn); /* to process the current and new insns */
    }
    if (MIR_call_code_p (code)) inline_p = TRUE;
    if ((MIR_int_branch_code_p (code) || code == MIR_JMP) && insn->ops[0].mode == MIR_OP_LABEL
        && skip_labels (next_insn, insn->ops[0].u.label) == insn->ops[0].u.label) {
      /* BR L|JMP L; <labels>L: => <labels>L: Also Remember signaling NAN*/
      MIR_remove_insn (ctx, func_item, insn);
    } else if (((code == MIR_MUL || code == MIR_MULS || code == MIR_MULO || code == MIR_MULOS
                 || code == MIR_DIV || code == MIR_DIVS)
                && insn->ops[2].mode == MIR_OP_INT && insn->ops[2].u.i == 1)
               || ((code == MIR_ADD || code == MIR_ADDS || code == MIR_SUB || code == MIR_SUBS
                    || code == MIR_OR || code == MIR_ORS || code == MIR_XOR || code == MIR_XORS
                    || code == MIR_LSH || code == MIR_LSHS || code == MIR_RSH || code == MIR_RSHS
                    || code == MIR_URSH || code == MIR_URSHS)
                   && insn->ops[2].mode == MIR_OP_INT && insn->ops[2].u.i == 0)) {
      if (!MIR_op_eq_p (ctx, insn->ops[0], insn->ops[1])) {
        next_insn = MIR_new_insn (ctx, MIR_MOV, insn->ops[0], insn->ops[1]);
        MIR_insert_insn_before (ctx, func_item, insn, next_insn);
      }
      MIR_remove_insn (ctx, func_item, insn);
    } else if (MIR_int_branch_code_p (code) && next_insn != NULL && next_insn->code == MIR_JMP
               && insn->ops[0].mode == MIR_OP_LABEL && next_insn->ops[0].mode == MIR_OP_LABEL
               && (skip_labels (next_insn->ops[0].u.label, insn->ops[0].u.label)
                     == insn->ops[0].u.label
                   || skip_labels (insn->ops[0].u.label, next_insn->ops[0].u.label)
                        == next_insn->ops[0].u.label)) {
      /* BR L1;JMP L2; L2:<labels>L1: or L1:<labels>L2: =>  JMP L2*/
      MIR_remove_insn (ctx, func_item, insn);
    } else if ((code == MIR_BT || code == MIR_BTS || code == MIR_BF || code == MIR_BFS)
               && insn->ops[1].mode == MIR_OP_INT
               && (insn->ops[1].u.i == 0 || insn->ops[1].u.i == 1)) {
      /* BT|BF L,zero|nonzero => nothing or JMP L */
      if ((code == MIR_BT || code == MIR_BTS) == (insn->ops[1].u.i == 1)) {
        new_insn = MIR_new_insn (ctx, MIR_JMP, insn->ops[0]);
        MIR_insert_insn_before (ctx, func_item, insn, new_insn);
        next_insn = new_insn;
      }
      MIR_remove_insn (ctx, func_item, insn);
      // ??? make imm always second,  what is about mem?
    } else if ((rev_code = MIR_reverse_branch_code (insn->code)) != MIR_INSN_BOUND
               && next_insn != NULL && next_insn->code == MIR_JMP
               && (next_next_insn = DLIST_NEXT (MIR_insn_t, next_insn)) != NULL
               && next_next_insn->code == MIR_LABEL && insn->ops[0].mode == MIR_OP_LABEL
               && skip_labels (next_next_insn, insn->ops[0].u.label) == insn->ops[0].u.label) {
      /* BCond L;JMP L2;<lables>L: => BNCond L2;<labels>L: */
      insn->ops[0] = next_insn->ops[0];
      insn->code = rev_code;
      MIR_remove_insn (ctx, func_item, next_insn);
      next_insn = insn;
    } else if (MIR_branch_code_p (code) && insn->ops[0].mode == MIR_OP_LABEL
               && (jmp_insn = skip_labels (insn->ops[0].u.label, NULL)) != NULL
               && jmp_insn->code == MIR_JMP && ++jmps_num < MAX_JUMP_CHAIN_LEN) {
      /* B L;...;L<labels>:JMP L2 => B L2; ... Constrain processing to avoid infinite loops */
      insn->ops[0] = jmp_insn->ops[0];
      next_insn = insn;
      continue;
    } else {
      if ((MIR_any_branch_code_p (code) && code != MIR_JMPI) || code == MIR_LADDR
          || code == MIR_PRBEQ || code == MIR_PRBNE) {
        size_t start_label_nop = 0, bound_label_nop = 1, n;

        if (code == MIR_LADDR) {
          start_label_nop = 1;
          bound_label_nop = 2;
        } else if (code == MIR_SWITCH) {
          start_label_nop = 1;
          bound_label_nop = start_label_nop + insn->nops - 1;
        }
        for (n = start_label_nop; n < bound_label_nop; n++) {
          label = last_label (insn->ops[n].u.label);
          if (label != insn->ops[n].u.label) insn->ops[n].u.label = label;
          mark_used_label (ctx, label);
        }
      }
      simplify_insn (ctx, func_item, insn, TRUE, mem_float_p);
    }
    jmps_num = 0;
  }
  make_one_ret (ctx, func_item);
  for (MIR_lref_data_t lref = func->first_lref; lref != NULL; lref = lref->next) {
    mark_used_label (ctx, lref->label);
    if (lref->label2 != NULL) mark_used_label (ctx, lref->label2);
  }
  remove_unused_and_enumerate_labels (ctx, func_item);
#if 0
  fprintf (stderr, "+++++ Function after simplification:\n");
  MIR_output_item (ctx, stderr, func_item);
#endif
  return inline_p;
}

static void set_inline_reg_map (MIR_context_t ctx, MIR_reg_t old_reg, MIR_reg_t new_reg) {
  while (VARR_LENGTH (MIR_reg_t, inline_reg_map) <= old_reg)
    VARR_PUSH (MIR_reg_t, inline_reg_map, 0);
  VARR_SET (MIR_reg_t, inline_reg_map, old_reg, new_reg);
}

#ifndef MIR_MAX_INSNS_FOR_INLINE
#define MIR_MAX_INSNS_FOR_INLINE 200
#endif

#ifndef MIR_MAX_INSNS_FOR_CALL_INLINE
#define MIR_MAX_INSNS_FOR_CALL_INLINE 50
#endif

#ifndef MIR_MAX_FUNC_INLINE_GROWTH
#define MIR_MAX_FUNC_INLINE_GROWTH 50
#endif

#ifndef MIR_MAX_CALLER_SIZE_FOR_ANY_GROWTH_INLINE
#define MIR_MAX_CALLER_SIZE_FOR_ANY_GROWTH_INLINE MIR_MAX_INSNS_FOR_INLINE
#endif

/* Simple alloca analysis.  Return top alloca insn with const size.
   If there are other allocas return true through
   non_top_alloca_p. Should we consider bstart/bend too?  */
static MIR_insn_t func_alloca_features (MIR_context_t ctx, MIR_func_t func, int *top_alloca_used_p,
                                        int *non_top_alloca_p, int64_t *alloca_size) {
  int set_top_alloca_p = TRUE;
  MIR_reg_t alloca_reg;
  MIR_op_t *op_ref;
  MIR_insn_t top_alloca = NULL, insn, prev_insn;

  *top_alloca_used_p = FALSE;
  if (non_top_alloca_p != NULL) *non_top_alloca_p = FALSE;
  for (insn = DLIST_HEAD (MIR_insn_t, func->insns); insn != NULL;
       insn = DLIST_NEXT (MIR_insn_t, insn)) {
    if (insn->code == MIR_LABEL && set_top_alloca_p) set_top_alloca_p = FALSE;
    if (insn->code != MIR_ALLOCA) {
      if (top_alloca == NULL || *top_alloca_used_p) continue;
      alloca_reg = top_alloca->ops[0].u.reg;
      for (size_t i = 0; i < insn->nops; i++)
        if ((insn->ops[i].mode == MIR_OP_REG && insn->ops[i].u.reg == alloca_reg)
            || (insn->ops[i].mode == MIR_OP_MEM
                && (insn->ops[i].u.mem.base == alloca_reg
                    || insn->ops[i].u.mem.index == alloca_reg))) {
          *top_alloca_used_p = TRUE;
          break;
        }
      continue;
    }
    op_ref = &insn->ops[1];
    if (insn->ops[1].mode == MIR_OP_REG && (prev_insn = DLIST_PREV (MIR_insn_t, insn)) != NULL
        && prev_insn->code == MIR_MOV && MIR_op_eq_p (ctx, prev_insn->ops[0], insn->ops[1]))
      op_ref = &prev_insn->ops[1];
    if (op_ref->mode != MIR_OP_INT && op_ref->mode != MIR_OP_UINT) op_ref = NULL;
    if (!set_top_alloca_p || op_ref == NULL) {
      if (non_top_alloca_p != NULL) *non_top_alloca_p = TRUE;
      if (top_alloca == NULL) return NULL;
    } else {
      top_alloca = insn;
      if (insn->ops[0].mode != MIR_OP_REG) *top_alloca_used_p = TRUE;
      set_top_alloca_p = FALSE;
      if (alloca_size != NULL) *alloca_size = op_ref->u.i;
    }
  }
  return top_alloca;
}

/* Generate block move only in simplified MIR.  ??? short move w/o loop. */
static long add_blk_move (MIR_context_t ctx, MIR_item_t func_item, MIR_insn_t before, MIR_op_t dest,
                          MIR_op_t src, size_t src_size, long label_num) {
  MIR_func_t func = func_item->u.func;
  size_t blk_size = (src_size + 7) / 8 * 8;
  MIR_insn_t insn;
  MIR_op_t size = MIR_new_reg_op (ctx, new_temp_reg (ctx, MIR_T_I64, func));

  insn = MIR_new_insn (ctx, MIR_MOV, size, MIR_new_int_op (ctx, blk_size));
  MIR_insert_insn_before (ctx, func_item, before, insn);
  insn = MIR_new_insn (ctx, MIR_ALLOCA, dest, size);
  MIR_insert_insn_before (ctx, func_item, before, insn);
  if (blk_size != 0) {
    MIR_reg_t addr_reg = new_temp_reg (ctx, MIR_T_I64, func);
    MIR_op_t addr = MIR_new_reg_op (ctx, addr_reg);
    MIR_op_t disp = MIR_new_reg_op (ctx, new_temp_reg (ctx, MIR_T_I64, func));
    MIR_op_t step = MIR_new_reg_op (ctx, new_temp_reg (ctx, MIR_T_I64, func));
    MIR_op_t temp = MIR_new_reg_op (ctx, new_temp_reg (ctx, MIR_T_I64, func));
    MIR_label_t loop = create_label (ctx, label_num++), skip = create_label (ctx, label_num++);

    insn = MIR_new_insn (ctx, MIR_MOV, disp, MIR_new_int_op (ctx, 0));
    MIR_insert_insn_before (ctx, func_item, before, insn);
    insn = MIR_new_insn (ctx, MIR_BLE, MIR_new_label_op (ctx, skip), size, disp);
    MIR_insert_insn_before (ctx, func_item, before, insn);
    MIR_insert_insn_before (ctx, func_item, before, loop);
    insn = MIR_new_insn (ctx, MIR_ADD, addr, src, disp);
    MIR_insert_insn_before (ctx, func_item, before, insn);
    insn = MIR_new_insn (ctx, MIR_MOV, temp, MIR_new_mem_op (ctx, MIR_T_I64, 0, addr_reg, 0, 1));
    MIR_insert_insn_before (ctx, func_item, before, insn);
    insn = MIR_new_insn (ctx, MIR_ADD, addr, dest, disp);
    MIR_insert_insn_before (ctx, func_item, before, insn);
    insn = MIR_new_insn (ctx, MIR_MOV, MIR_new_mem_op (ctx, MIR_T_I64, 0, addr_reg, 0, 1), temp);
    MIR_insert_insn_before (ctx, func_item, before, insn);
    insn = MIR_new_insn (ctx, MIR_MOV, step, MIR_new_int_op (ctx, 8));
    MIR_insert_insn_before (ctx, func_item, before, insn);
    insn = MIR_new_insn (ctx, MIR_ADD, disp, disp, step);
    MIR_insert_insn_before (ctx, func_item, before, insn);
    insn = MIR_new_insn (ctx, MIR_BLT, MIR_new_label_op (ctx, loop), disp, size);
    MIR_insert_insn_before (ctx, func_item, before, insn);
    MIR_insert_insn_before (ctx, func_item, before, skip);
  }
  return label_num;
}

static void rename_regs (MIR_context_t ctx, MIR_func_t func, MIR_func_t called_func,
                         VARR (MIR_var_t) * vars, size_t nvars) {
  char buff[50];
  const char *hard_reg_name;
  MIR_var_t var;
  MIR_type_t type;
  MIR_reg_t old_reg, new_reg;

  if (vars == NULL) return;
  for (size_t i = 0; i < nvars; i++) {
    VARR_TRUNC (char, temp_string, 0);
    sprintf (buff, ".c%d_", func->n_inlines);
    VARR_PUSH_ARR (char, temp_string, buff, strlen (buff));
    var = VARR_GET (MIR_var_t, vars, i);
    type
      = (var.type == MIR_T_F || var.type == MIR_T_D || var.type == MIR_T_LD ? var.type : MIR_T_I64);
    old_reg = MIR_reg (ctx, var.name, called_func);
    VARR_PUSH_ARR (char, temp_string, var.name, strlen (var.name) + 1);
    if ((hard_reg_name = MIR_reg_hard_reg_name (ctx, old_reg, called_func)) != NULL) {
      new_reg
        = MIR_new_global_func_reg (ctx, func, type, VARR_ADDR (char, temp_string), hard_reg_name);
    } else {
      new_reg = MIR_new_func_reg (ctx, func, type, VARR_ADDR (char, temp_string));
    }
    set_inline_reg_map (ctx, old_reg, new_reg);
  }
}

static void change_inline_insn_regs (MIR_context_t ctx, MIR_insn_t new_insn) {
  size_t i, actual_nops;
  actual_nops = MIR_insn_nops (ctx, new_insn);
  for (i = 0; i < actual_nops; i++) {
    switch (new_insn->ops[i].mode) {
    case MIR_OP_REG:
      new_insn->ops[i].u.reg = VARR_GET (MIR_reg_t, inline_reg_map, new_insn->ops[i].u.reg);
      break;
    case MIR_OP_MEM:
      if (new_insn->ops[i].u.mem.base != 0)
        new_insn->ops[i].u.mem.base
          = VARR_GET (MIR_reg_t, inline_reg_map, new_insn->ops[i].u.mem.base);
      if (new_insn->ops[i].u.mem.index != 0)
        new_insn->ops[i].u.mem.index
          = VARR_GET (MIR_reg_t, inline_reg_map, new_insn->ops[i].u.mem.index);
      break;
    default: /* do nothing */ break;
    }
  }
}

/* Only simplified code should be inlined because we need already
   extensions and one return.  */
static void process_inlines (MIR_context_t ctx, MIR_item_t func_item) {
  int non_top_alloca_p, func_top_alloca_used_p, called_func_top_alloca_used_p;
  int64_t alloca_size, alloca_align, max_func_top_alloca_align;
  int64_t init_func_top_alloca_size, curr_func_top_alloca_size, max_func_top_alloca_size;
  size_t i, nargs, arg_num;
  MIR_type_t type, *res_types;
  MIR_var_t var;
  MIR_reg_t ret_reg, temp_reg;
  MIR_insn_t func_top_alloca, called_func_top_alloca, new_called_func_top_alloca = NULL;
  MIR_insn_t func_insn, head_func_insn, next_func_insn;
  MIR_insn_t call, insn, prev_insn, new_insn, ret_insn, anchor, stop_insn;
  MIR_item_t called_func_item;
  MIR_func_t func, called_func;
  size_t original_func_insns_num, func_insns_num, called_func_insns_num;

  mir_assert (func_item->item_type == MIR_func_item);
  vn_empty (ctx);
  func = func_item->u.func;
  original_func_insns_num = func_insns_num = DLIST_LENGTH (MIR_insn_t, func->insns);
  func_top_alloca = func_alloca_features (ctx, func, &func_top_alloca_used_p, NULL, &alloca_size);
  mir_assert (func_top_alloca != NULL || !func_top_alloca_used_p);
  init_func_top_alloca_size = curr_func_top_alloca_size = max_func_top_alloca_size = 0;
  max_func_top_alloca_align = 0;
  if (func_top_alloca != NULL && func_top_alloca_used_p)
    init_func_top_alloca_size = max_func_top_alloca_size = curr_func_top_alloca_size
      = get_alloca_size_align (alloca_size, &max_func_top_alloca_align);
  VARR_TRUNC (MIR_insn_t, anchors, 0);
  VARR_TRUNC (size_t, alloca_sizes, 0);
  VARR_TRUNC (MIR_insn_t, cold_insns, 0);
  for (head_func_insn = func_insn = DLIST_HEAD (MIR_insn_t, func->insns); func_insn != NULL;
       func_insn = next_func_insn) {
    inline_insns_before++;
    inline_insns_after++;
    while (VARR_LENGTH (MIR_insn_t, anchors) != 0 && VARR_LAST (MIR_insn_t, anchors) == func_insn) {
      VARR_POP (MIR_insn_t, anchors);
      curr_func_top_alloca_size = VARR_POP (size_t, alloca_sizes);
    }
    next_func_insn = DLIST_NEXT (MIR_insn_t, func_insn);
    if (func_insn->code == MIR_LABEL) func_insn->ops[0].u.i = new_label_num++;
    if (!MIR_call_code_p (func_insn->code)) continue;
    call = func_insn;
    if (call->ops[1].mode != MIR_OP_REF) {
      simplify_op (ctx, func_item, func_insn, 1, FALSE, func_insn->code, FALSE, TRUE);
      continue;
    }
    called_func_item = call->ops[1].u.ref;
    while (called_func_item != NULL
           && (called_func_item->item_type == MIR_import_item
               || called_func_item->item_type == MIR_export_item
               || called_func_item->item_type == MIR_forward_item))
      called_func_item = called_func_item->ref_def;
    if (called_func_item == NULL || called_func_item->item_type != MIR_func_item
        || func_item == called_func_item) { /* Simplify function operand in the inline insn */
      simplify_op (ctx, func_item, func_insn, 1, FALSE, func_insn->code, FALSE, TRUE);
      continue;
    }
    called_func = called_func_item->u.func;
    called_func_insns_num = DLIST_LENGTH (MIR_insn_t, called_func->insns);
    if (called_func->first_lref != NULL || called_func->vararg_p || called_func->jret_p
        || called_func_insns_num > (func_insn->code != MIR_CALL ? MIR_MAX_INSNS_FOR_INLINE
                                                                : MIR_MAX_INSNS_FOR_CALL_INLINE)
        || (func_insns_num > MIR_MAX_FUNC_INLINE_GROWTH * original_func_insns_num / 100
            && func_insns_num > MIR_MAX_CALLER_SIZE_FOR_ANY_GROWTH_INLINE)) {
      simplify_op (ctx, func_item, func_insn, 1, FALSE, func_insn->code, FALSE, TRUE);
      continue;
    }
    func_insns_num += called_func_insns_num;
    inlined_calls++;
    res_types = call->ops[0].u.ref->u.proto->res_types;
    prev_insn = DLIST_PREV (MIR_insn_t, call);
    if ((anchor = DLIST_NEXT (MIR_insn_t, call)) == NULL) {
      anchor = MIR_new_label (ctx);
      MIR_insert_insn_after (ctx, func_item, call, anchor);
    }
    func->n_inlines++;
    rename_regs (ctx, func, called_func, called_func->vars,
                 VARR_LENGTH (MIR_var_t, called_func->vars));
    rename_regs (ctx, func, called_func, called_func->global_vars,
                 called_func->global_vars == NULL
                   ? 0
                   : VARR_LENGTH (MIR_var_t, called_func->global_vars));
    nargs = called_func->nargs;
    for (i = 2 + called_func->nres, arg_num = 0; arg_num < nargs && i < call->nops;
         i++, arg_num++) { /* Parameter passing */
      MIR_op_t op = call->ops[i];
      var = VARR_GET (MIR_var_t, called_func->vars, arg_num);
      type = (var.type == MIR_T_F || var.type == MIR_T_D || var.type == MIR_T_LD ? var.type
                                                                                 : MIR_T_I64);
      const char *old_var_name = var.name;
      MIR_reg_t old_reg = MIR_reg (ctx, old_var_name, called_func);
      MIR_reg_t new_reg = VARR_GET (MIR_reg_t, inline_reg_map, old_reg);

      mir_assert (!MIR_all_blk_type_p (type) || (op.mode == MIR_OP_MEM && type == MIR_T_I64));
      if (MIR_blk_type_p (var.type)) { /* alloca and block move: */
        new_label_num
          = add_blk_move (ctx, func_item, anchor, MIR_new_reg_op (ctx, new_reg),
                          MIR_new_reg_op (ctx, op.u.mem.base), var.size, (long) new_label_num);
      } else {
        if (var.type == MIR_T_RBLK) op = MIR_new_reg_op (ctx, op.u.mem.base);
        new_insn = MIR_new_insn (ctx, get_type_move_code (type), MIR_new_reg_op (ctx, new_reg), op);
        MIR_insert_insn_before (ctx, func_item, anchor, new_insn);
      }
    }
    /* ??? No frame only alloca */
    VARR_PUSH (MIR_insn_t, anchors, anchor);
    VARR_PUSH (size_t, alloca_sizes, curr_func_top_alloca_size);
    /* Add new insns: */
    ret_reg = 0;
    called_func_top_alloca = func_alloca_features (ctx, called_func, &called_func_top_alloca_used_p,
                                                   &non_top_alloca_p, &alloca_size);
    if (called_func_top_alloca != NULL && called_func_top_alloca_used_p) {
      alloca_size = get_alloca_size_align (alloca_size, &alloca_align);
      if (max_func_top_alloca_align < alloca_align) {
        max_func_top_alloca_align = alloca_align;
        curr_func_top_alloca_size
          = (curr_func_top_alloca_size + alloca_align - 1) / alloca_align * alloca_align;
      }
      curr_func_top_alloca_size += alloca_size;
      if (max_func_top_alloca_size < curr_func_top_alloca_size)
        max_func_top_alloca_size = curr_func_top_alloca_size;
    }
    VARR_TRUNC (MIR_insn_t, temp_insns, 0);
    VARR_TRUNC (MIR_insn_t, labels, 0);
    VARR_TRUNC (uint8_t, temp_data, 0);
    stop_insn = NULL;
    if (!non_top_alloca_p) { /* store cold code when we have no BSTART/BEND */
      for (insn = DLIST_TAIL (MIR_insn_t, called_func->insns); insn != NULL;
           insn = DLIST_PREV (MIR_insn_t, insn)) {
        if (insn->code == MIR_RET || insn->code == MIR_JRET) break;
        inline_insns_after++;
        new_insn = MIR_copy_insn (ctx, insn);
        change_inline_insn_regs (ctx, new_insn);
        store_labels_for_duplication (ctx, labels, temp_insns, insn, new_insn);
        VARR_PUSH (MIR_insn_t, cold_insns, new_insn);
      }
      mir_assert (insn != NULL);
      stop_insn = DLIST_NEXT (MIR_insn_t, insn);
    }
    for (insn = DLIST_HEAD (MIR_insn_t, called_func->insns); insn != stop_insn;
         insn = DLIST_NEXT (MIR_insn_t, insn)) {
      mir_assert (insn->code != MIR_JRET);
      inline_insns_after++;
      new_insn = MIR_copy_insn (ctx, insn);
      /* va insns are possible here as va_list can be passed as arg */
      if (insn == called_func_top_alloca) new_called_func_top_alloca = new_insn;
      change_inline_insn_regs (ctx, new_insn);
      if (new_insn->code != MIR_RET) {
        MIR_insert_insn_before (ctx, func_item, anchor, new_insn);
        store_labels_for_duplication (ctx, labels, temp_insns, insn, new_insn);
      } else {
        size_t actual_nops = MIR_insn_nops (ctx, insn);
        /* [J]RET should be the last insn extracting cold code */
        mir_assert (DLIST_NEXT (MIR_insn_t, insn) == stop_insn && call->ops[0].mode == MIR_OP_REF
                    && call->ops[0].u.ref->item_type == MIR_proto_item);
        mir_assert (called_func->nres == actual_nops);
        ret_insn = new_insn;
        for (i = 0; i < actual_nops; i++) {
          mir_assert (ret_insn->ops[i].mode == MIR_OP_REG);
          ret_reg = ret_insn->ops[i].u.reg;
          new_insn = MIR_new_insn (ctx, get_type_move_code (res_types[i]), call->ops[i + 2],
                                   MIR_new_reg_op (ctx, ret_reg));
          MIR_insert_insn_before (ctx, func_item, anchor, new_insn);
        }
        MIR_free (ctx->alloc, ret_insn);
      }
    }
    redirect_duplicated_labels (ctx, labels, temp_insns);
    if (non_top_alloca_p) {
      temp_reg = new_temp_reg (ctx, MIR_T_I64, func);
      new_insn = MIR_new_insn (ctx, MIR_BSTART, MIR_new_reg_op (ctx, temp_reg));
      MIR_insert_insn_after (ctx, func_item, call, new_insn);
      new_insn = MIR_new_insn (ctx, MIR_BEND, MIR_new_reg_op (ctx, temp_reg));
      MIR_insert_insn_before (ctx, func_item, anchor, new_insn);
    }
    if (called_func_top_alloca != NULL) {
      if (called_func_top_alloca_used_p) {
        func_top_alloca_used_p = TRUE;
        if (func_top_alloca == NULL) {
          temp_reg = new_temp_reg (ctx, MIR_T_I64, func);
          func_top_alloca = MIR_new_insn (ctx, MIR_ALLOCA, new_called_func_top_alloca->ops[0],
                                          MIR_new_reg_op (ctx, temp_reg));
          if (head_func_insn->code != MIR_LABEL)
            MIR_insert_insn_before (ctx, func_item, head_func_insn, func_top_alloca);
          else
            MIR_insert_insn_after (ctx, func_item, head_func_insn, func_top_alloca);
          init_func_top_alloca_size = 0;
          new_insn
            = MIR_new_insn (ctx, MIR_MOV, MIR_new_reg_op (ctx, temp_reg), MIR_new_int_op (ctx, 0));
          MIR_insert_insn_before (ctx, func_item, func_top_alloca, new_insn);
        }
        if (curr_func_top_alloca_size - alloca_size == 0) {
          new_insn = MIR_new_insn (ctx, MIR_MOV, new_called_func_top_alloca->ops[0],
                                   func_top_alloca->ops[0]);
        } else {
          temp_reg = new_temp_reg (ctx, MIR_T_I64, func);
          new_insn
            = MIR_new_insn (ctx, MIR_PTR32 ? MIR_ADDS : MIR_ADD, new_called_func_top_alloca->ops[0],
                            func_top_alloca->ops[0], MIR_new_reg_op (ctx, temp_reg));
          MIR_insert_insn_after (ctx, func_item, call, new_insn);
          new_insn = MIR_new_insn (ctx, MIR_MOV, MIR_new_reg_op (ctx, temp_reg),
                                   MIR_new_int_op (ctx, curr_func_top_alloca_size - alloca_size));
        }
        MIR_insert_insn_after (ctx, func_item, call, new_insn);
      }
      if (head_func_insn == new_called_func_top_alloca)
        head_func_insn = DLIST_NEXT (MIR_insn_t, head_func_insn);
      MIR_remove_insn (ctx, func_item, new_called_func_top_alloca);
    }
    if (head_func_insn == call) head_func_insn = DLIST_NEXT (MIR_insn_t, head_func_insn);
    MIR_remove_insn (ctx, func_item, call);
    if (head_func_insn == call) head_func_insn = DLIST_HEAD (MIR_insn_t, func->insns);
    next_func_insn = (prev_insn == NULL ? DLIST_HEAD (MIR_insn_t, func->insns)
                                        : DLIST_NEXT (MIR_insn_t, prev_insn));
  }
  mir_assert (VARR_LENGTH (MIR_insn_t, anchors) == 0 && VARR_LENGTH (size_t, alloca_sizes) == 0);
  if (func_top_alloca != NULL) {
    if (!func_top_alloca_used_p) {
      MIR_remove_insn (ctx, func_item, func_top_alloca);
    } else if (max_func_top_alloca_size != init_func_top_alloca_size) {
      temp_reg = new_temp_reg (ctx, MIR_T_I64, func);
      new_insn = MIR_new_insn (ctx, MIR_MOV, MIR_new_reg_op (ctx, temp_reg),
                               MIR_new_int_op (ctx, max_func_top_alloca_size));
      func_top_alloca->ops[1] = MIR_new_reg_op (ctx, temp_reg);
      MIR_insert_insn_before (ctx, func_item, func_top_alloca, new_insn);
    }
  }
  while (VARR_LENGTH (MIR_insn_t, cold_insns) != 0) {
    insn = VARR_POP (MIR_insn_t, cold_insns);
    if (insn->code == MIR_LABEL) insn->ops[0].u.i = new_label_num++;
    MIR_append_insn (ctx, func_item, insn);
  }
  if (curr_label_num < new_label_num) curr_label_num = new_label_num;
}

/* New Page */

const char *_MIR_uniq_string (MIR_context_t ctx, const char *str) { return get_ctx_str (ctx, str); }

/* The next two function can be called any time relative to
   load/linkage.  You can also call them many times for the same name
   but you should always use the same prototype or/and addr for the
   same proto/func name.  */
MIR_item_t _MIR_builtin_proto (MIR_context_t ctx, MIR_module_t module, const char *name,
                               size_t nres, MIR_type_t *res_types, size_t nargs, ...) {
  size_t i;
  va_list argp;
  MIR_var_t *args = alloca (nargs * sizeof (MIR_var_t));
  MIR_item_t proto_item;
  MIR_module_t saved_module;

  va_start (argp, nargs);
  saved_module = curr_module;
  for (i = 0; i < nargs; i++) {
    args[i].type = va_arg (argp, MIR_type_t);
    args[i].name = va_arg (argp, const char *);
  }
  va_end (argp);
  name = _MIR_uniq_string (ctx, name);
  proto_item = item_tab_find (ctx, name, module);
  if (proto_item != NULL) {
    if (proto_item->item_type == MIR_proto_item && proto_item->u.proto->nres == nres
        && VARR_LENGTH (MIR_var_t, proto_item->u.proto->args) == nargs) {
      for (i = 0; i < nres; i++)
        if (res_types[i] != proto_item->u.proto->res_types[i]) break;
      if (i >= nres) {
        for (i = 0; i < nargs; i++)
          if (args[i].type != VARR_GET (MIR_var_t, proto_item->u.proto->args, i).type) break;
        if (i >= nargs) {
          return proto_item;
        }
      }
    }
    MIR_get_error_func (ctx) (MIR_repeated_decl_error,
                              "_MIR_builtin_proto: proto item %s was already defined differently",
                              name);
  }
  saved_module = curr_module;
  curr_module = module;
  proto_item = MIR_new_proto_arr (ctx, name, nres, res_types, nargs, args);
  DLIST_REMOVE (MIR_item_t, curr_module->items, proto_item);
  DLIST_PREPEND (MIR_item_t, curr_module->items, proto_item); /* make it first in the list */
  curr_module = saved_module;
  return proto_item;
}

MIR_item_t _MIR_builtin_func (MIR_context_t ctx, MIR_module_t module, const char *name,
                              void *addr) {
  MIR_item_t item, ref_item;
  MIR_module_t saved_module = curr_module;

  name = _MIR_uniq_string (ctx, name);
  if ((ref_item = item_tab_find (ctx, name, &environment_module)) != NULL) {
    if (ref_item->item_type != MIR_import_item || ref_item->addr != addr)
      MIR_get_error_func (ctx) (MIR_repeated_decl_error,
                                "_MIR_builtin_func: func %s has already another address", name);
  } else {
    curr_module = &environment_module;
    /* Use import for builtin func: */
    item = new_export_import_forward (ctx, name, MIR_import_item, "import", TRUE);
    HTAB_DO (MIR_item_t, module_item_tab, item, HTAB_INSERT, ref_item);
    mir_assert (item == ref_item);
    DLIST_APPEND (MIR_item_t, environment_module.items, item);
    ref_item->addr = addr;
    curr_module = saved_module;
  }
  if ((item = item_tab_find (ctx, name, module)) != NULL) {
    if (item->item_type != MIR_import_item || item->addr != addr || item->ref_def != ref_item)
      MIR_get_error_func (
        ctx) (MIR_repeated_decl_error,
              "_MIR_builtin_func: func name %s was already defined differently in the "
              "module",
              name);
  } else {
    curr_module = module;
    item = new_export_import_forward (ctx, name, MIR_import_item, "import", FALSE);
    DLIST_REMOVE (MIR_item_t, curr_module->items, item);
    DLIST_PREPEND (MIR_item_t, curr_module->items, item); /* make it first in the list */
    item->addr = ref_item->addr;
    item->ref_def = ref_item;
    curr_module = saved_module;
  }
  return item;
}

/* New Page */
/* This page is for dealing with generated machine code */

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>

static size_t mem_page_size () {
  return sysconf (_SC_PAGE_SIZE);
}
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static size_t mem_page_size () {
  SYSTEM_INFO sysInfo;
  GetSystemInfo (&sysInfo);
  return sysInfo.dwPageSize;
}
#endif

struct code_holder {
  uint8_t *start, *free, *bound;
};

typedef struct code_holder code_holder_t;

DEF_VARR (code_holder_t);

struct machine_code_ctx {
  VARR (code_holder_t) * code_holders;
  size_t page_size;
};

#define code_holders ctx->machine_code_ctx->code_holders
#define page_size ctx->machine_code_ctx->page_size

static code_holder_t *get_last_code_holder (MIR_context_t ctx, size_t size) {
  uint8_t *mem;
  size_t len, npages;
  code_holder_t ch, *ch_ptr;

  if ((len = VARR_LENGTH (code_holder_t, code_holders)) > 0) {
    ch_ptr = VARR_ADDR (code_holder_t, code_holders) + len - 1;
    ch_ptr->free = (uint8_t *) ((uint64_t) (ch_ptr->free + 15) / 16 * 16); /* align */
    if (ch_ptr->free + size <= ch_ptr->bound) return ch_ptr;
  }
  npages = (size + page_size) / page_size;
  len = page_size * npages;
  mem = (uint8_t *) MIR_mem_map (ctx->code_alloc, len);
  if (mem == MAP_FAILED) return NULL;
  ch.start = mem;
  ch.free = mem;
  ch.bound = mem + len;
  VARR_PUSH (code_holder_t, code_holders, ch);
  len = VARR_LENGTH (code_holder_t, code_holders);
  return VARR_ADDR (code_holder_t, code_holders) + len - 1;
}

void _MIR_flush_code_cache (void *start, void *bound) {
#if defined(__GNUC__) && !defined(__MIRC__)
  __builtin___clear_cache (start, bound);
#endif
}

#if !defined(MIR_BOOTSTRAP) || !defined(__APPLE__) || !defined(__aarch64__)
void _MIR_set_code (MIR_code_alloc_t code_alloc, size_t prot_start, size_t prot_len,
                    uint8_t *base, size_t nloc, const MIR_code_reloc_t *relocs,
                    size_t reloc_size) {
  MIR_mem_protect (code_alloc, (uint8_t *) prot_start, prot_len, PROT_WRITE_EXEC);
  if (reloc_size == 0) {
    for (size_t i = 0; i < nloc; i++)
      memcpy (base + relocs[i].offset, &relocs[i].value, sizeof (void *));
  } else {
    for (size_t i = 0; i < nloc; i++) memcpy (base + relocs[i].offset, relocs[i].value, reloc_size);
  }
  MIR_mem_protect (code_alloc, (uint8_t *) prot_start, prot_len, PROT_READ_EXEC);
}
#endif

static uint8_t *add_code (MIR_context_t ctx MIR_UNUSED, code_holder_t *ch_ptr, const uint8_t *code,
                          size_t code_len) {
  uint8_t *mem = ch_ptr->free;

  ch_ptr->free += code_len;
  mir_assert (ch_ptr->free <= ch_ptr->bound);
  MIR_code_reloc_t reloc;
  reloc.offset = 0;
  reloc.value = code;
  _MIR_set_code (ctx->code_alloc, (size_t) ch_ptr->start, ch_ptr->bound - ch_ptr->start, mem, 1, &reloc, code_len);
  _MIR_flush_code_cache (mem, ch_ptr->free);
  return mem;
}

uint8_t *_MIR_publish_code (MIR_context_t ctx, const uint8_t *code,
                            size_t code_len) { /* thread safe */
  code_holder_t *ch_ptr;
  uint8_t *res = NULL;

  if ((ch_ptr = get_last_code_holder (ctx, code_len)) != NULL)
    res = add_code (ctx, ch_ptr, code, code_len);
  return res;
}

uint8_t *_MIR_publish_code_by_addr (MIR_context_t ctx, void *addr, const uint8_t *code,
                                    size_t code_len) {
  code_holder_t *ch_ptr = get_last_code_holder (ctx, 0);
  uint8_t *res = NULL;

  if (ch_ptr != NULL && ch_ptr->free == addr && ch_ptr->free + code_len <= ch_ptr->bound)
    res = add_code (ctx, ch_ptr, code, code_len);
  return res;
}

void _MIR_change_code (MIR_context_t ctx, uint8_t *addr, const uint8_t *code,
                       size_t code_len) { /* thread safe */
  MIR_code_reloc_t reloc;
  size_t len, start;

  start = (size_t) addr / page_size * page_size;
  len = (size_t) addr + code_len - start;
  reloc.offset = 0;
  reloc.value = code;
  _MIR_set_code (ctx->code_alloc, start, len, addr, 1, &reloc, code_len);
  _MIR_flush_code_cache (addr, addr + code_len);
}

void _MIR_update_code_arr (MIR_context_t ctx, uint8_t *base, size_t nloc,
                           const MIR_code_reloc_t *relocs) { /* thread safe */
  size_t i, len, start, max_offset = 0;

  mir_assert (relocs != NULL);
  for (i = 0; i < nloc; i++)
    if (max_offset < relocs[i].offset) max_offset = relocs[i].offset;
  start = (size_t) base / page_size * page_size;
  len = (size_t) base + max_offset + sizeof (void *) - start;
  _MIR_set_code (ctx->code_alloc, start, len, base, nloc, relocs, 0);
  _MIR_flush_code_cache (base, base + max_offset + sizeof (void *));
}

void _MIR_update_code (MIR_context_t ctx, uint8_t *base, size_t nloc, ...) { /* thread safe */
  va_list args;
  MIR_code_reloc_t relocs[20];
  if (nloc >= 20)
    MIR_get_error_func (ctx) (MIR_wrong_param_value_error, "_MIR_update_code: too many locations");
  va_start (args, nloc);
  for (size_t i = 0; i < nloc; i++) {
    relocs[i].offset = va_arg (args, size_t);
    relocs[i].value = va_arg (args, void *);
  }
  va_end (args);
  _MIR_update_code_arr (ctx, base, nloc, relocs);
}

uint8_t *_MIR_get_new_code_addr (MIR_context_t ctx, size_t size) {
  code_holder_t *ch_ptr = get_last_code_holder (ctx, size);

  return ch_ptr == NULL ? NULL : ch_ptr->free;
}

static void code_init (MIR_context_t ctx) {
  if ((ctx->machine_code_ctx = MIR_malloc (ctx->alloc, sizeof (struct machine_code_ctx))) == NULL)
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for ctx");
  page_size = mem_page_size ();
  VARR_CREATE (code_holder_t, code_holders, ctx->alloc, 128);
}

static void code_finish (MIR_context_t ctx) {
  while (VARR_LENGTH (code_holder_t, code_holders) != 0) {
    code_holder_t ch = VARR_POP (code_holder_t, code_holders);
    MIR_mem_unmap (ctx->code_alloc, ch.start, ch.bound - ch.start);
  }
  VARR_DESTROY (code_holder_t, code_holders);
  MIR_free (ctx->alloc, ctx->machine_code_ctx);
  ctx->machine_code_ctx = NULL;
}

/* New Page */

#if !MIR_NO_IO || !MIR_NO_SCAN
static void process_reserved_name (const char *s, const char *prefix, uint32_t *max_num) {
  char *end;
  uint32_t num;
  size_t len = strlen (prefix);

  if (strncmp (s, prefix, len) != 0) return;
  num = strtoul (s + len, &end, 10);
  if (*end != '\0') return;
  if (*max_num < num) *max_num = num;
}
#endif

#if !MIR_NO_IO

/* Input/output of binary MIR.  Major goal of binary MIR is fast
   reading, not compression ratio.  Text MIR major CPU time consumer
   is a scanner.  Mostly in reading binary MIR we skip the scanner
   part by using tokens.  Each token starts with a tag which describes
   subsequent optional bytes.  */

#define TAG_EL(t) TAG_##t
#define REP_SEP ,
typedef enum {
  TAG_EL (U0),
  REP8 (TAG_EL, U1, U2, U3, U4, U5, U6, U7, U8),
  REP8 (TAG_EL, I1, I2, I3, I4, I5, I6, I7, I8),
  REP3 (TAG_EL, F, D, LD),                   /* 4, 8, 16 bytes for floating point numbers */
  REP4 (TAG_EL, REG1, REG2, REG3, REG4),     /* Reg string number in 1, 2, 3, 4 bytes */
  REP4 (TAG_EL, NAME1, NAME2, NAME3, NAME4), /* Name string number in 1, 2, 3, 4 bytes */
  REP4 (TAG_EL, STR1, STR2, STR3, STR4),     /* String number in 1, 2, 3, 4 bytes */
  REP4 (TAG_EL, LAB1, LAB2, LAB3, LAB4),     /* Label number in 1, 2, 3, 4 bytes */
  /* Tags for memory operands.  The memory address parts are the subsequent tokens */
  REP4 (TAG_EL, MEM_DISP, MEM_BASE, MEM_INDEX, MEM_DISP_BASE),
  REP3 (TAG_EL, MEM_DISP_INDEX, MEM_BASE_INDEX, MEM_DISP_BASE_INDEX),
  /* MIR types. The same order as MIR types: */
  REP8 (TAG_EL, TI8, TU8, TI16, TU16, TI32, TU32, TI64, TU64),
  REP5 (TAG_EL, TF, TD, TP, TV, TBLOCK),
  TAG_EL (TRBLOCK) = TAG_EL (TBLOCK) + MIR_BLK_NUM,
  TAG_EL (EOI),
  TAG_EL (EOFILE), /* end of insn with variable number operands (e.g. a call) or end of file */
  REP4 (TAG_EL, ALIAS_MEM_DISP, ALIAS_MEM_BASE, ALIAS_MEM_INDEX, ALIAS_MEM_DISP_BASE),
  REP3 (TAG_EL, ALIAS_MEM_DISP_INDEX, ALIAS_MEM_BASE_INDEX, ALIAS_MEM_DISP_BASE_INDEX),
  TAG_EL (LAST) = TAG_EL (ALIAS_MEM_DISP_BASE_INDEX),
  /* unsigned integer 0..127 is kept in one byte.  The most significant bit of the byte is 1: */
  U0_MASK = 0x7f,
  U0_FLAG = 0x80,
} bin_tag_t;
#undef REP_SEP

/* MIR binary format:

   VERSION
   NSTR
   (string)*
   ( ((label)* (insn code) (operand)* | STRN=(func|global|local|import|export|forward|<data>) ...)
     EOI?
   )* EOF

   where
   o VERSION and NSTR are unsigned tokens
   o insn code is unsigned token
   o string is string number tokens
   o operand is unsigned, signed, float, double, string, label, memory tokens
   o EOI, EOF - tokens for end of insn (optional for most insns) and end of file
*/

static const int CURR_BIN_VERSION = 1;

DEF_VARR (MIR_str_t);
DEF_VARR (uint64_t);
DEF_VARR (MIR_label_t);

struct io_ctx {
  FILE *io_file;
  int (*io_writer) (MIR_context_t, uint8_t);
  int (*io_reader) (MIR_context_t);
  struct reduce_data *io_reduce_data;
  VARR (MIR_var_t) * proto_vars;
  VARR (MIR_type_t) * proto_types;
  VARR (MIR_op_t) * read_insn_ops;
  VARR (string_t) * output_strings;
  HTAB (string_t) * output_string_tab;
  VARR (MIR_str_t) * bin_strings;
  VARR (uint64_t) * insn_label_string_nums;
  VARR (MIR_label_t) * func_labels;
  size_t output_insns_len, output_labs_len;
  size_t output_regs_len, output_mem_len, output_int_len, output_float_len;
};

#define io_file ctx->io_ctx->io_file
#define io_writer ctx->io_ctx->io_writer
#define io_reader ctx->io_ctx->io_reader
#define io_reduce_data ctx->io_ctx->io_reduce_data
#define proto_vars ctx->io_ctx->proto_vars
#define proto_types ctx->io_ctx->proto_types
#define read_insn_ops ctx->io_ctx->read_insn_ops
#define output_strings ctx->io_ctx->output_strings
#define output_string_tab ctx->io_ctx->output_string_tab
#define bin_strings ctx->io_ctx->bin_strings
#define insn_label_string_nums ctx->io_ctx->insn_label_string_nums
#define func_labels ctx->io_ctx->func_labels
#define output_insns_len ctx->io_ctx->output_insns_len
#define output_labs_len ctx->io_ctx->output_labs_len
#define output_regs_len ctx->io_ctx->output_regs_len
#define output_mem_len ctx->io_ctx->output_mem_len
#define output_int_len ctx->io_ctx->output_int_len
#define output_float_len ctx->io_ctx->output_float_len

typedef reduce_writer_t writer_func_t;

static size_t put_byte (MIR_context_t ctx, writer_func_t writer, int ch) {
  if (writer == NULL) return 0;
#ifdef MIR_NO_BIN_COMPRESSION
  io_writer (ctx, ch);
#else
  reduce_encode_put (io_reduce_data, ch);
#endif
  return 1;
}

static size_t uint_length (uint64_t u) {
  size_t n;

  if (u <= 127) return 0;
  for (n = 0; u != 0; n++) u >>= CHAR_BIT;
  return n;
}

static size_t put_uint (MIR_context_t ctx, writer_func_t writer, uint64_t u, int nb) {
  if (writer == NULL) return 0;
  for (int n = 0; n < nb; n++) {
    put_byte (ctx, writer, u & 0xff);
    u >>= CHAR_BIT;
  }
  return nb;
}

static size_t int_length (int64_t i) {
  uint64_t u = i;
  size_t n = 0;

  for (n = 0; u != 0; n++) u >>= CHAR_BIT;
  return n == 0 ? 1 : n;
}

static size_t put_int (MIR_context_t ctx, writer_func_t writer, int64_t i, int nb) {
  return put_uint (ctx, writer, (uint64_t) i, nb);
}

static size_t put_float (MIR_context_t ctx, writer_func_t writer, float fl) {
  union {
    uint32_t u;
    float f;
  } u;

  if (writer == NULL) return 0;
  u.f = fl;
  return put_uint (ctx, writer, u.u, sizeof (uint32_t));
}

static size_t put_double (MIR_context_t ctx, writer_func_t writer, double d) {
  union {
    uint64_t u;
    double d;
  } u;

  if (writer == NULL) return 0;
  u.d = d;
  return put_uint (ctx, writer, u.u, sizeof (uint64_t));
}

static size_t put_ldouble (MIR_context_t ctx, writer_func_t writer, long double ld) {
  union {
    uint64_t u[2];
    long double ld;
  } u;
  size_t len;

  if (writer == NULL) return 0;
  u.ld = ld;
  len = put_uint (ctx, writer, u.u[0], sizeof (uint64_t));
  return put_uint (ctx, writer, u.u[1], sizeof (uint64_t)) + len;
}

/* Write binary MIR */

static size_t write_int (MIR_context_t ctx, writer_func_t writer, int64_t i) {
  size_t nb, len;

  if (writer == NULL) return 0;
  nb = int_length (i);
  assert (nb > 0);
  put_byte (ctx, writer, TAG_I1 + (int) nb - 1);
  len = put_int (ctx, writer, i, (int) nb) + 1;
  output_int_len += len;
  return len;
}

static size_t write_uint (MIR_context_t ctx, writer_func_t writer, uint64_t u) {
  size_t nb, len;

  if (writer == NULL) return 0;
  if ((nb = uint_length (u)) == 0) {
    put_byte (ctx, writer, (int) (0x80 | u));
    return 1;
  }
  put_byte (ctx, writer, TAG_U1 + (int) nb - 1);
  len = put_uint (ctx, writer, u, (int) nb) + 1;
  output_int_len += len;
  return len;
}

static size_t write_float (MIR_context_t ctx, writer_func_t writer, float fl) {
  size_t len;

  if (writer == NULL) return 0;
  put_byte (ctx, writer, TAG_F);
  len = put_float (ctx, writer, fl) + 1;
  output_float_len += len;
  return len;
}

static size_t write_double (MIR_context_t ctx, writer_func_t writer, double d) {
  size_t len;

  if (writer == NULL) return 0;
  put_byte (ctx, writer, TAG_D);
  len = put_double (ctx, writer, d) + 1;
  output_float_len += len;
  return len;
}

static size_t write_ldouble (MIR_context_t ctx, writer_func_t writer, long double ld) {
  size_t len;

  if (writer == NULL) return 0;
  put_byte (ctx, writer, TAG_LD);
  len = put_ldouble (ctx, writer, ld) + 1;
  output_int_len += len;
  return len;
}

static size_t write_str_tag (MIR_context_t ctx, writer_func_t writer, MIR_str_t str,
                             bin_tag_t start_tag) {
  size_t nb;
  int ok_p;
  string_t string;

  if (writer == NULL) {
    string_store (ctx, &output_strings, &output_string_tab, str);
    return 0;
  }
  ok_p = string_find (&output_strings, &output_string_tab, str, &string);
  mir_assert (ok_p && string.num >= 1);
  nb = uint_length (string.num - 1);
  mir_assert (nb <= 4);
  if (nb == 0) nb = 1;
  put_byte (ctx, writer, start_tag + (int) nb - 1);
  return put_uint (ctx, writer, string.num - 1, (int) nb) + 1;
}

static size_t write_str (MIR_context_t ctx, writer_func_t writer, MIR_str_t str) {
  return write_str_tag (ctx, writer, str, TAG_STR1);
}
static size_t write_name (MIR_context_t ctx, writer_func_t writer, const char *name) {
  return write_str_tag (ctx, writer, (MIR_str_t){strlen (name) + 1, name}, TAG_NAME1);
}

static size_t write_reg (MIR_context_t ctx, writer_func_t writer, const char *reg_name) {
  size_t len = write_str_tag (ctx, writer, (MIR_str_t){strlen (reg_name) + 1, reg_name}, TAG_REG1);

  output_regs_len += len;
  return len;
}

static size_t write_type (MIR_context_t ctx, writer_func_t writer, MIR_type_t t) {
  return put_byte (ctx, writer, TAG_TI8 + (t - MIR_T_I8));
}

static size_t write_lab (MIR_context_t ctx, writer_func_t writer, MIR_label_t lab) {
  size_t nb, len;
  uint64_t lab_num;

  if (writer == NULL) return 0;
  lab_num = lab->ops[0].u.u;
  nb = uint_length (lab_num);
  mir_assert (nb <= 4);
  if (nb == 0) nb = 1;
  put_byte (ctx, writer, TAG_LAB1 + (int) nb - 1);
  len = put_uint (ctx, writer, lab_num, (int) nb) + 1;
  output_labs_len += len;
  return len;
}

static size_t write_op (MIR_context_t ctx, writer_func_t writer, MIR_func_t func, MIR_op_t op) {
  switch (op.mode) {
  case MIR_OP_REG: return write_reg (ctx, writer, MIR_reg_name (ctx, op.u.reg, func));
  case MIR_OP_INT: return write_int (ctx, writer, op.u.i);
  case MIR_OP_UINT: return write_uint (ctx, writer, op.u.u);
  case MIR_OP_FLOAT: return write_float (ctx, writer, op.u.f);
  case MIR_OP_DOUBLE: return write_double (ctx, writer, op.u.d);
  case MIR_OP_LDOUBLE: return write_ldouble (ctx, writer, op.u.ld);
  case MIR_OP_MEM: {
    bin_tag_t tag;
    size_t len;
    int alias_p = op.u.mem.alias != 0 || op.u.mem.nonalias != 0;

    if (op.u.mem.disp != 0) {
      if (op.u.mem.base != 0)
        tag = op.u.mem.index != 0
                ? (alias_p ? TAG_ALIAS_MEM_DISP_BASE_INDEX : TAG_MEM_DISP_BASE_INDEX)
                : (alias_p ? TAG_ALIAS_MEM_DISP_BASE : TAG_MEM_DISP_BASE);
      else
        tag = op.u.mem.index != 0 ? (alias_p ? TAG_ALIAS_MEM_DISP_INDEX : TAG_MEM_DISP_INDEX)
                                  : (alias_p ? TAG_ALIAS_MEM_DISP : TAG_MEM_DISP);
    } else if (op.u.mem.base != 0) {
      tag = op.u.mem.index != 0 ? (alias_p ? TAG_ALIAS_MEM_BASE_INDEX : TAG_MEM_BASE_INDEX)
                                : (alias_p ? TAG_ALIAS_MEM_BASE : TAG_MEM_BASE);
    } else if (op.u.mem.index != 0) {
      tag = alias_p ? TAG_ALIAS_MEM_INDEX : TAG_MEM_INDEX;
    } else {
      tag = alias_p ? TAG_ALIAS_MEM_DISP : TAG_MEM_DISP;
    }
    put_byte (ctx, writer, tag);
    len = write_type (ctx, writer, op.u.mem.type) + 1;
    if (op.u.mem.disp != 0 || (op.u.mem.base == 0 && op.u.mem.index == 0))
      write_int (ctx, writer, op.u.mem.disp);
    if (op.u.mem.base != 0) write_reg (ctx, writer, MIR_reg_name (ctx, op.u.mem.base, func));
    if (op.u.mem.index != 0) {
      len += write_reg (ctx, writer, MIR_reg_name (ctx, op.u.mem.index, func));
      len += write_uint (ctx, writer, op.u.mem.scale);
    }
    if (alias_p) {
      len += write_name (ctx, writer, MIR_alias_name (ctx, op.u.mem.alias));
      len += write_name (ctx, writer, MIR_alias_name (ctx, op.u.mem.nonalias));
    }
    output_mem_len += len;
    return len;
  }
  case MIR_OP_REF: return write_name (ctx, writer, MIR_item_name (ctx, op.u.ref));
  case MIR_OP_STR: return write_str (ctx, writer, op.u.str);
  case MIR_OP_LABEL: return write_lab (ctx, writer, op.u.label);
  default: mir_assert (FALSE); return 0;
  }
}

static size_t write_insn (MIR_context_t ctx, writer_func_t writer, MIR_func_t func,
                          MIR_insn_t insn) {
  size_t i, nops;
  MIR_insn_code_t code = insn->code;
  size_t len;

  if (code == MIR_UNSPEC || code == MIR_USE || code == MIR_PHI)
    MIR_get_error_func (ctx) (MIR_binary_io_error,
                              "UNSPEC, USE, or PHI is not portable and can not be output");
  if (code == MIR_LABEL) return write_lab (ctx, writer, insn);
  nops = MIR_insn_nops (ctx, insn);
  len = write_uint (ctx, writer, code);
  for (i = 0; i < nops; i++) len += write_op (ctx, writer, func, insn->ops[i]);
  if (insn_descs[code].op_modes[0] == MIR_OP_BOUND) {
    /* first operand mode is undefined if it is a variable operand insn */
    mir_assert (MIR_call_code_p (code) || code == MIR_RET || code == MIR_SWITCH);
    put_byte (ctx, writer, TAG_EOI);
    len++;
  }
  output_insns_len += len;
  return len;
}

static size_t write_vars (MIR_context_t ctx, writer_func_t writer, MIR_func_t func,
                          VARR (MIR_var_t) * vars, size_t start, size_t vars_num,
                          const char *prefix) {
  if (vars_num == 0 || vars == NULL) return 0;
  size_t len = 0;
  int first_p = TRUE;
  for (size_t i = 0; i < vars_num; i++) {
    MIR_var_t var = VARR_GET (MIR_var_t, vars, i + start);
    if (first_p) len += write_name (ctx, writer, prefix);
    first_p = FALSE;
    len += write_type (ctx, writer, var.type);
    len += write_name (ctx, writer, var.name);
    MIR_reg_t reg = MIR_reg (ctx, var.name, func);
    const char *hard_reg_name = MIR_reg_hard_reg_name (ctx, reg, func);
    if (hard_reg_name != NULL) len += write_name (ctx, writer, hard_reg_name);
  }
  len += put_byte (ctx, writer, TAG_EOI);
  return len;
}

static size_t write_item (MIR_context_t ctx, writer_func_t writer, MIR_item_t item) {
  MIR_insn_t insn;
  MIR_func_t func;
  MIR_proto_t proto;
  MIR_var_t var;
  size_t i, vars_num, len = 0;

  if (item->item_type == MIR_import_item) {
    len += write_name (ctx, writer, "import");
    len += write_name (ctx, writer, item->u.import_id);
    return len;
  }
  if (item->item_type == MIR_export_item) {
    len += write_name (ctx, writer, "export");
    len += write_name (ctx, writer, item->u.export_id);
    return len;
  }
  if (item->item_type == MIR_forward_item) {
    len += write_name (ctx, writer, "forward");
    len += write_name (ctx, writer, item->u.forward_id);
    return len;
  }
  if (item->item_type == MIR_bss_item) {
    if (item->u.bss->name == NULL) {
      len += write_name (ctx, writer, "bss");
    } else {
      len += write_name (ctx, writer, "nbss");
      len += write_name (ctx, writer, item->u.bss->name);
    }
    len += write_uint (ctx, writer, item->u.bss->len);
    return len;
  }
  if (item->item_type == MIR_ref_data_item) {
    if (item->u.ref_data->name == NULL) {
      len += write_name (ctx, writer, "ref");
    } else {
      len += write_name (ctx, writer, "nref");
      len += write_name (ctx, writer, item->u.ref_data->name);
    }
    len += write_name (ctx, writer, MIR_item_name (ctx, item->u.ref_data->ref_item));
    len += write_int (ctx, writer, item->u.ref_data->disp);
    return len;
  }
  if (item->item_type == MIR_lref_data_item) {
    if (item->u.lref_data->name == NULL) {
      len += write_name (ctx, writer, "lref");
    } else {
      len += write_name (ctx, writer, "nlref");
      len += write_name (ctx, writer, item->u.lref_data->name);
    }
    mir_assert (item->u.lref_data->label->ops[0].mode == MIR_OP_INT);
    mir_assert (item->u.lref_data->label2 == NULL
                || (item->u.lref_data->label2->ops[0].mode == MIR_OP_INT
                    && item->u.lref_data->label2->ops[0].u.i >= 0));
    len += write_int (ctx, writer, item->u.lref_data->label->ops[0].u.i);
    if (item->u.lref_data->label2 == NULL) {
      len += write_int (ctx, writer, -1);
    } else {
      mir_assert (item->u.lref_data->label2->ops[0].mode == MIR_OP_INT
                  && item->u.lref_data->label2->ops[0].u.i >= 0);
      len += write_int (ctx, writer, item->u.lref_data->label2->ops[0].u.i);
    }
    len += write_int (ctx, writer, item->u.lref_data->disp);
    return len;
  }
  if (item->item_type == MIR_expr_data_item) {
    if (item->u.expr_data->name == NULL) {
      len += write_name (ctx, writer, "expr");
    } else {
      len += write_name (ctx, writer, "nexpr");
      len += write_name (ctx, writer, item->u.expr_data->name);
    }
    len += write_name (ctx, writer, MIR_item_name (ctx, item->u.expr_data->expr_item));
    return len;
  }
  if (item->item_type == MIR_data_item) {
    MIR_data_t data = item->u.data;

    if (data->name == NULL) {
      len += write_name (ctx, writer, "data");
    } else {
      len += write_name (ctx, writer, "ndata");
      len += write_name (ctx, writer, data->name);
    }
    write_type (ctx, writer, data->el_type);
    for (i = 0; i < data->nel; i++) switch (data->el_type) {
      case MIR_T_I8: len += write_int (ctx, writer, ((int8_t *) data->u.els)[i]); break;
      case MIR_T_U8: len += write_uint (ctx, writer, ((uint8_t *) data->u.els)[i]); break;
      case MIR_T_I16: len += write_int (ctx, writer, ((int16_t *) data->u.els)[i]); break;
      case MIR_T_U16: len += write_uint (ctx, writer, ((uint16_t *) data->u.els)[i]); break;
      case MIR_T_I32: len += write_int (ctx, writer, ((int32_t *) data->u.els)[i]); break;
      case MIR_T_U32: len += write_uint (ctx, writer, ((uint32_t *) data->u.els)[i]); break;
      case MIR_T_I64: len += write_int (ctx, writer, ((int64_t *) data->u.els)[i]); break;
      case MIR_T_U64: len += write_uint (ctx, writer, ((uint64_t *) data->u.els)[i]); break;
      case MIR_T_F: len += write_float (ctx, writer, ((float *) data->u.els)[i]); break;
      case MIR_T_D: len += write_double (ctx, writer, ((double *) data->u.els)[i]); break;
      case MIR_T_LD:
        len += write_ldouble (ctx, writer, ((long double *) data->u.els)[i]);
        break;
        /* only ptr as ref ??? */
      case MIR_T_P: len += write_uint (ctx, writer, ((uintptr_t *) data->u.els)[i]); break;
      default: mir_assert (FALSE);
      }
    len += put_byte (ctx, writer, TAG_EOI);
    return len;
  }
  if (item->item_type == MIR_proto_item) {
    proto = item->u.proto;
    len += write_name (ctx, writer, "proto");
    len += write_name (ctx, writer, proto->name);
    len += write_uint (ctx, writer, proto->vararg_p != 0);
    len += write_uint (ctx, writer, proto->nres);
    for (i = 0; i < proto->nres; i++) write_type (ctx, writer, proto->res_types[i]);
    for (i = 0; i < VARR_LENGTH (MIR_var_t, proto->args); i++) {
      var = VARR_GET (MIR_var_t, proto->args, i);
      len += write_type (ctx, writer, var.type);
      len += write_name (ctx, writer, var.name);
      if (MIR_all_blk_type_p (var.type)) len += write_uint (ctx, writer, var.size);
    }
    len += put_byte (ctx, writer, TAG_EOI);
    return len;
  }
  func = item->u.func;
  len += write_name (ctx, writer, "func");
  len += write_name (ctx, writer, func->name);
  len += write_uint (ctx, writer, func->vararg_p != 0);
  len += write_uint (ctx, writer, func->nres);
  for (i = 0; i < func->nres; i++) len += write_type (ctx, writer, func->res_types[i]);
  for (i = 0; i < func->nargs; i++) {
    var = VARR_GET (MIR_var_t, func->vars, i);
    len += write_type (ctx, writer, var.type);
    len += write_name (ctx, writer, var.name);
    if (MIR_all_blk_type_p (var.type)) len += write_uint (ctx, writer, var.size);
  }
  len += put_byte (ctx, writer, TAG_EOI);
  vars_num = VARR_LENGTH (MIR_var_t, func->vars) - func->nargs;
  len += write_vars (ctx, writer, func, func->vars, func->nargs, vars_num, "local");
  len += write_vars (ctx, writer, func, func->global_vars, 0,
                     func->global_vars == NULL ? 0 : VARR_LENGTH (MIR_var_t, func->global_vars),
                     "global");
  for (insn = DLIST_HEAD (MIR_insn_t, func->insns); insn != NULL;
       insn = DLIST_NEXT (MIR_insn_t, insn))
    len += write_insn (ctx, writer, func, insn);
  len += write_name (ctx, writer, "endfunc");
  return len;
}

static size_t write_module (MIR_context_t ctx, writer_func_t writer, MIR_module_t module) {
  size_t len = write_name (ctx, writer, "module");

  len += write_name (ctx, writer, module->name);
  for (MIR_item_t item = DLIST_HEAD (MIR_item_t, module->items); item != NULL;
       item = DLIST_NEXT (MIR_item_t, item))
    len += write_item (ctx, writer, item);
  len += write_name (ctx, writer, "endmodule");
  return len;
}

static size_t write_modules (MIR_context_t ctx, writer_func_t writer, MIR_module_t module) {
  size_t len = 0;

  for (MIR_module_t m = DLIST_HEAD (MIR_module_t, all_modules); m != NULL;
       m = DLIST_NEXT (MIR_module_t, m))
    if (module == NULL || m == module) len += write_module (ctx, writer, m);
  return len;
}

static size_t reduce_writer (const void *start, size_t len, void *aux_data) {
  MIR_context_t ctx = aux_data;
  size_t i, n = 0;

  for (i = n = 0; i < len; i++, n++)
    if (io_writer (ctx, ((uint8_t *) start)[i]) == EOF) break;
  return n;
}

void MIR_write_module_with_func (MIR_context_t ctx, int (*const writer) (MIR_context_t, uint8_t),
                                 MIR_module_t module) {
  size_t MIR_UNUSED len;
  size_t str_len;

  io_writer = writer;
#ifndef MIR_NO_BIN_COMPRESSION
  if ((io_reduce_data = reduce_encode_start (ctx->alloc, reduce_writer, ctx)) == NULL)
    MIR_get_error_func (ctx) (MIR_binary_io_error, "can not alloc data for MIR binary compression");
#endif
  output_insns_len = output_labs_len = 0;
  output_regs_len = output_mem_len = output_int_len = output_float_len = 0;
  string_init (ctx->alloc, &output_strings, &output_string_tab);
  write_modules (ctx, NULL, module); /* store strings */
  len = write_uint (ctx, reduce_writer, CURR_BIN_VERSION);
  str_len = write_uint (ctx, reduce_writer, VARR_LENGTH (string_t, output_strings) - 1);
  for (size_t i = 1; i < VARR_LENGTH (string_t, output_strings); i++) { /* output strings */
    MIR_str_t str = VARR_GET (string_t, output_strings, i).str;

    str_len += write_uint (ctx, reduce_writer, str.len);
    for (size_t j = 0; j < str.len; j++) {
      put_byte (ctx, reduce_writer, str.s[j]);
      str_len++;
    }
  }
  len += write_modules (ctx, reduce_writer, module) + str_len;
#if 0
  fprintf (stderr,
           "Overall output length = %lu.  Number of strings = %lu.\n"
           "Lengths of: strings = %lu, insns = %lu, labs = %lu,\n"
           "   reg ops = %lu, mem ops = %lu, int ops = %lu, float ops = %lu\n",
           len, VARR_LENGTH (string_t, output_strings), str_len, output_insns_len, output_labs_len,
           output_regs_len, output_mem_len, output_int_len, output_float_len);
#endif
  put_byte (ctx, reduce_writer, TAG_EOFILE);
  string_finish (ctx->alloc, &output_strings, &output_string_tab);
#ifndef MIR_NO_BIN_COMPRESSION
  if (!reduce_encode_finish (ctx->alloc, io_reduce_data))
    MIR_get_error_func (ctx) (MIR_binary_io_error, "error in writing MIR binary");
#endif
}

void MIR_write_with_func (MIR_context_t ctx, int (*const writer) (MIR_context_t, uint8_t)) {
  MIR_write_module_with_func (ctx, writer, NULL);
}

static int file_writer (MIR_context_t ctx, uint8_t byte) { return fputc (byte, io_file); }

void MIR_write_module (MIR_context_t ctx, FILE *f, MIR_module_t module) {
  io_file = f;
  MIR_write_module_with_func (ctx, file_writer, module);
}

void MIR_write (MIR_context_t ctx, FILE *f) { MIR_write_module (ctx, f, NULL); }

/* New Page */

static int get_byte (MIR_context_t ctx) {
#ifdef MIR_NO_BIN_COMPRESSION
  int c = io_reader (ctx);
#else
  int c = reduce_decode_get (io_reduce_data);
#endif

  if (c == EOF) MIR_get_error_func (ctx) (MIR_binary_io_error, "unfinished binary MIR");
  return c;
}

typedef union {
  uint64_t u;
  int64_t i;
  float f;
  double d;
  long double ld;
  MIR_type_t t;
  MIR_reg_t reg;
} token_attr_t;

static uint64_t get_uint (MIR_context_t ctx, int nb) {
  uint64_t res = 0;

  for (int i = 0; i < nb; i++) res |= (uint64_t) get_byte (ctx) << (i * CHAR_BIT);
  return res;
}

static int64_t get_int (MIR_context_t ctx, int nb) { return (int64_t) get_uint (ctx, nb); }

static float get_float (MIR_context_t ctx) {
  union {
    uint32_t u;
    float f;
  } u;

  u.u = (uint32_t) get_uint (ctx, (int) sizeof (uint32_t));
  return u.f;
}

static double get_double (MIR_context_t ctx) {
  union {
    uint64_t u;
    double d;
  } u;

  u.u = get_uint (ctx, sizeof (uint64_t));
  return u.d;
}

static long double get_ldouble (MIR_context_t ctx) {
  union {
    uint64_t u[2];
    long double ld;
  } u;

  u.u[0] = get_uint (ctx, sizeof (uint64_t));
  u.u[1] = get_uint (ctx, sizeof (uint64_t));
  return u.ld;
}

static MIR_str_t to_str (MIR_context_t ctx, uint64_t str_num) {
  if (str_num >= VARR_LENGTH (MIR_str_t, bin_strings))
    MIR_get_error_func (ctx) (MIR_binary_io_error, "wrong string num %lu", str_num);
  return VARR_GET (MIR_str_t, bin_strings, str_num);
}

static MIR_reg_t to_reg (MIR_context_t ctx, uint64_t reg_str_num, MIR_item_t func) {
  const char *s = to_str (ctx, reg_str_num).s;

  process_reserved_name (s, TEMP_REG_NAME_PREFIX, &func->u.func->last_temp_num);
  return MIR_reg (ctx, s, func->u.func);
}

static MIR_label_t to_lab (MIR_context_t ctx, uint64_t lab_num) {
  MIR_label_t lab;

  while (lab_num >= VARR_LENGTH (MIR_label_t, func_labels))
    VARR_PUSH (MIR_label_t, func_labels, NULL);
  if ((lab = VARR_GET (MIR_label_t, func_labels, lab_num)) != NULL) return lab;
  lab = create_label (ctx, lab_num);
  VARR_SET (MIR_label_t, func_labels, lab_num, lab);
  return lab;
}

static int64_t read_int (MIR_context_t ctx, const char *err_msg) {
  int c = get_byte (ctx);

  if (TAG_I1 > c || c > TAG_I8) MIR_get_error_func (ctx) (MIR_binary_io_error, err_msg);
  return get_int (ctx, c - TAG_I1 + 1);
}

static uint64_t read_uint (MIR_context_t ctx, const char *err_msg) {
  int c = get_byte (ctx);

  if (c & U0_FLAG) return c & U0_MASK;
  if (TAG_U1 > c || c > TAG_U8) MIR_get_error_func (ctx) (MIR_binary_io_error, err_msg);
  return get_uint (ctx, c - TAG_U1 + 1);
}

static void read_all_strings (MIR_context_t ctx, uint64_t nstr) {
  int c;
  MIR_str_t str;
  uint64_t len, l;

  VARR_TRUNC (MIR_str_t, bin_strings, 0);
  for (uint64_t i = 0; i < nstr; i++) {
    VARR_TRUNC (char, temp_string, 0);
    len = read_uint (ctx, "wrong string length");
    for (l = 0; l < len; l++) {
      c = get_byte (ctx);
      VARR_PUSH (char, temp_string, c);
    }
    str.s = VARR_ADDR (char, temp_string);
    str.len = len;
    str = get_ctx_string (ctx, str).str;
    VARR_PUSH (MIR_str_t, bin_strings, str);
  }
}

static MIR_type_t tag_type (bin_tag_t tag) { return (MIR_type_t) (tag - TAG_TI8) + MIR_T_I8; }

static MIR_type_t read_type (MIR_context_t ctx, const char *err_msg) {
  int c = get_byte (ctx);

  if (TAG_TI8 > c || c > TAG_TRBLOCK) MIR_get_error_func (ctx) (MIR_binary_io_error, err_msg);
  return tag_type (c);
}

static const char *read_name (MIR_context_t ctx, MIR_module_t module, const char *err_msg) {
  int c = get_byte (ctx);
  const char *s;

  if (TAG_NAME1 > c || c > TAG_NAME4) MIR_get_error_func (ctx) (MIR_binary_io_error, err_msg);
  s = to_str (ctx, get_uint (ctx, c - TAG_NAME1 + 1)).s;
  if (module != NULL) process_reserved_name (s, TEMP_ITEM_NAME_PREFIX, &module->last_temp_item_num);
  return s;
}

#define TAG_CASE(t) case TAG_##t:
#define REP_SEP
static bin_tag_t read_token (MIR_context_t ctx, token_attr_t *attr) {
  int c = get_byte (ctx);

  if (c & U0_FLAG) {
    attr->u = c & U0_MASK;
    return TAG_U0;
  }
  switch (c) {
    REP8 (TAG_CASE, U1, U2, U3, U4, U5, U6, U7, U8)
    attr->u = get_uint (ctx, c - TAG_U1 + 1);
    break;
    REP8 (TAG_CASE, I1, I2, I3, I4, I5, I6, I7, I8)
    attr->i = get_int (ctx, c - TAG_I1 + 1);
    break;
    TAG_CASE (F)
    attr->f = get_float (ctx);
    break;
    TAG_CASE (D)
    attr->d = get_double (ctx);
    break;
    TAG_CASE (LD)
    attr->ld = get_ldouble (ctx);
    break;
    REP4 (TAG_CASE, REG1, REG2, REG3, REG4)
    attr->u = get_uint (ctx, c - TAG_REG1 + 1);
    break;
    REP4 (TAG_CASE, NAME1, NAME2, NAME3, NAME4)
    attr->u = get_uint (ctx, c - TAG_NAME1 + 1);
    break;
    REP4 (TAG_CASE, STR1, STR2, STR3, STR4)
    attr->u = get_uint (ctx, c - TAG_STR1 + 1);
    break;
    REP4 (TAG_CASE, LAB1, LAB2, LAB3, LAB4)
    attr->u = get_uint (ctx, c - TAG_LAB1 + 1);
    break;
    REP6 (TAG_CASE, MEM_DISP, MEM_BASE, MEM_INDEX, MEM_DISP_BASE, MEM_DISP_INDEX, MEM_BASE_INDEX)
    REP3 (TAG_CASE, MEM_DISP_BASE_INDEX, EOI, EOFILE)
    REP4 (TAG_CASE, ALIAS_MEM_DISP, ALIAS_MEM_BASE, ALIAS_MEM_INDEX, ALIAS_MEM_DISP_BASE)
    REP3 (TAG_CASE, ALIAS_MEM_DISP_INDEX, ALIAS_MEM_BASE_INDEX, ALIAS_MEM_DISP_BASE_INDEX)
    break;
    REP8 (TAG_CASE, TI8, TU8, TI16, TU16, TI32, TU32, TI64, TU64)
    REP5 (TAG_CASE, TF, TD, TP, TV, TRBLOCK)
    attr->t = (MIR_type_t) (c - TAG_TI8) + MIR_T_I8;
    break;
  default:
    if (TAG_TBLOCK <= c && c < TAG_TBLOCK + MIR_BLK_NUM) {
      attr->t = (MIR_type_t) (c - TAG_TBLOCK) + MIR_T_BLK;
      break;
    }
    MIR_get_error_func (ctx) (MIR_binary_io_error, "wrong tag %d", c);
  }
  return c;
}

static MIR_disp_t read_disp (MIR_context_t ctx) {
  bin_tag_t tag;
  token_attr_t attr;

  tag = read_token (ctx, &attr);
  if (TAG_I1 > tag || tag > TAG_I8)
    MIR_get_error_func (ctx) (MIR_binary_io_error, "memory disp has wrong tag %d", tag);
  return attr.i;
}

static MIR_reg_t read_reg (MIR_context_t ctx, MIR_item_t func) {
  bin_tag_t tag;
  token_attr_t attr;

  tag = read_token (ctx, &attr);
  if (TAG_REG1 > tag || tag > TAG_REG4)
    MIR_get_error_func (ctx) (MIR_binary_io_error, "register has wrong tag %d", tag);
  return to_reg (ctx, attr.u, func);
}

static int read_operand (MIR_context_t ctx, MIR_op_t *op, MIR_item_t func) {
  bin_tag_t tag;
  token_attr_t attr;
  MIR_type_t t;
  MIR_disp_t disp;
  MIR_reg_t base, index;
  MIR_scale_t scale;
  int alias_p = FALSE;
  const char *name;

  tag = read_token (ctx, &attr);
  switch (tag) {
    TAG_CASE (U0)
    REP8 (TAG_CASE, U1, U2, U3, U4, U5, U6, U7, U8) *op = MIR_new_uint_op (ctx, attr.u);
    break;
    REP8 (TAG_CASE, I1, I2, I3, I4, I5, I6, I7, I8)
    *op = MIR_new_int_op (ctx, attr.i);
    break;
    TAG_CASE (F)
    *op = MIR_new_float_op (ctx, attr.f);
    break;
    TAG_CASE (D)
    *op = MIR_new_double_op (ctx, attr.d);
    break;
    TAG_CASE (LD)
    *op = MIR_new_ldouble_op (ctx, attr.ld);
    break;
    REP4 (TAG_CASE, REG1, REG2, REG3, REG4)
    *op = MIR_new_reg_op (ctx, to_reg (ctx, attr.u, func));
    break;
    REP4 (TAG_CASE, NAME1, NAME2, NAME3, NAME4) {
      name = to_str (ctx, attr.u).s;
      MIR_item_t item = item_tab_find (ctx, name, func->module);

      if (item == NULL) MIR_get_error_func (ctx) (MIR_binary_io_error, "not found item %s", name);
      *op = MIR_new_ref_op (ctx, item);
      break;
    }
    REP4 (TAG_CASE, STR1, STR2, STR3, STR4)
    *op = MIR_new_str_op (ctx, to_str (ctx, attr.u));
    break;
    REP4 (TAG_CASE, LAB1, LAB2, LAB3, LAB4)
    *op = MIR_new_label_op (ctx, to_lab (ctx, attr.u));
    break;
    REP7 (TAG_CASE, ALIAS_MEM_DISP, ALIAS_MEM_BASE, ALIAS_MEM_INDEX, ALIAS_MEM_DISP_BASE,
          ALIAS_MEM_DISP_INDEX, ALIAS_MEM_BASE_INDEX, ALIAS_MEM_DISP_BASE_INDEX)
    alias_p = TRUE;
    /* falls through */
  case TAG_MEM_DISP:
    REP6 (TAG_CASE, MEM_BASE, MEM_INDEX, MEM_DISP_BASE, MEM_DISP_INDEX, MEM_BASE_INDEX,
          MEM_DISP_BASE_INDEX)
    t = read_type (ctx, "wrong memory type");
    disp = (tag == TAG_MEM_DISP || tag == TAG_MEM_DISP_BASE || tag == TAG_MEM_DISP_INDEX
                || tag == TAG_MEM_DISP_BASE_INDEX || tag == TAG_ALIAS_MEM_DISP
                || tag == TAG_ALIAS_MEM_DISP_BASE || tag == TAG_ALIAS_MEM_DISP_INDEX
                || tag == TAG_ALIAS_MEM_DISP_BASE_INDEX
              ? read_disp (ctx)
              : 0);
    base = (tag == TAG_MEM_BASE || tag == TAG_MEM_DISP_BASE || tag == TAG_MEM_BASE_INDEX
                || tag == TAG_MEM_DISP_BASE_INDEX || tag == TAG_ALIAS_MEM_BASE
                || tag == TAG_ALIAS_MEM_DISP_BASE || tag == TAG_ALIAS_MEM_BASE_INDEX
                || tag == TAG_ALIAS_MEM_DISP_BASE_INDEX
              ? read_reg (ctx, func)
              : 0);
    index = 0;
    scale = 0;
    if (tag == TAG_MEM_INDEX || tag == TAG_MEM_DISP_INDEX || tag == TAG_MEM_BASE_INDEX
        || tag == TAG_MEM_DISP_BASE_INDEX || tag == TAG_ALIAS_MEM_INDEX
        || tag == TAG_ALIAS_MEM_DISP_INDEX || tag == TAG_ALIAS_MEM_BASE_INDEX
        || tag == TAG_ALIAS_MEM_DISP_BASE_INDEX) {
      index = read_reg (ctx, func);
      scale = (MIR_scale_t) read_uint (ctx, "wrong memory index scale");
    }
    *op = MIR_new_mem_op (ctx, t, disp, base, index, scale);
    if (alias_p) {
      name = read_name (ctx, func->module, "wrong alias name");
      if (strcmp (name, "") != 0) op->u.mem.alias = MIR_alias (ctx, name);
      name = read_name (ctx, func->module, "wrong nonalias name");
      if (strcmp (name, "") != 0) op->u.mem.nonalias = MIR_alias (ctx, name);
    }
    break;
  case TAG_EOI: return FALSE;
  default: mir_assert (FALSE);
  }
  return TRUE;
}
#undef REP_SEP

static int func_proto_read (MIR_context_t ctx, MIR_module_t module, uint64_t *nres_ptr) {
  bin_tag_t tag;
  token_attr_t attr;
  MIR_var_t var;
  int vararg_p = read_uint (ctx, "wrong vararg flag") != 0;
  uint64_t i, nres = read_uint (ctx, "wrong func nres");

  VARR_TRUNC (MIR_type_t, proto_types, 0);
  for (i = 0; i < nres; i++) {
    tag = read_token (ctx, &attr);
    if (TAG_TI8 > tag || tag > TAG_TRBLOCK)
      MIR_get_error_func (ctx) (MIR_binary_io_error, "wrong prototype result type tag %d", tag);
    VARR_PUSH (MIR_type_t, proto_types, tag_type (tag));
  }
  VARR_TRUNC (MIR_var_t, proto_vars, 0);
  for (;;) {
    tag = read_token (ctx, &attr);
    if (tag == TAG_EOI) break;
    if (TAG_TI8 > tag || tag > TAG_TRBLOCK)
      MIR_get_error_func (ctx) (MIR_binary_io_error, "wrong prototype arg type tag %d", tag);
    var.type = tag_type (tag);
    var.name = read_name (ctx, module, "wrong arg name");
    if (MIR_all_blk_type_p (var.type)) var.size = read_uint (ctx, "wrong block arg size");
    VARR_PUSH (MIR_var_t, proto_vars, var);
  }
  *nres_ptr = nres;
  return vararg_p;
}

#ifndef MIR_NO_BIN_COMPRESSION
static size_t reduce_reader (void *start, size_t len, void *data) {
  MIR_context_t ctx = data;
  size_t i;
  int c;

  for (i = 0; i < len && (c = io_reader (ctx)) != EOF; i++) ((char *) start)[i] = c;
  return i;
}
#endif

void MIR_read_with_func (MIR_context_t ctx, int (*const reader) (MIR_context_t)) {
  int version, global_p, nlref_p;
  bin_tag_t tag, type_tag;
  token_attr_t attr;
  MIR_label_t lab, lab2;
  uint64_t nstr, nres, u;
  int64_t i;
  MIR_op_t op;
  size_t n, nop;
  const char *name, *item_name;
  MIR_module_t module;
  MIR_item_t func, item;

  io_reader = reader;
#ifndef MIR_NO_BIN_COMPRESSION
  if ((io_reduce_data = reduce_decode_start (ctx->alloc, reduce_reader, ctx)) == NULL)
    MIR_get_error_func (ctx) (MIR_binary_io_error,
                              "can not alloc data for MIR binary decompression");
#endif
  version = (int) read_uint (ctx, "wrong header");
  if (version > CURR_BIN_VERSION)
    MIR_get_error_func (ctx) (MIR_binary_io_error,
                              "can not read version %d MIR binary: expected %d or less", version,
                              CURR_BIN_VERSION);
  nstr = read_uint (ctx, "wrong header");
  read_all_strings (ctx, nstr);
  module = NULL;
  func = NULL;
  for (;;) {
    VARR_TRUNC (uint64_t, insn_label_string_nums, 0);
    tag = read_token (ctx, &attr);
    while (TAG_LAB1 <= tag && tag <= TAG_LAB4) {
      VARR_PUSH (uint64_t, insn_label_string_nums, attr.u);
      tag = read_token (ctx, &attr);
    }
    VARR_TRUNC (MIR_op_t, read_insn_ops, 0);
    if (TAG_NAME1 <= tag && tag <= TAG_NAME4) {
      name = to_str (ctx, attr.u).s;
      if (strcmp (name, "module") == 0) {
        name = read_name (ctx, module, "wrong module name");
        if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "insn label before module %s", name);
        if (module != NULL)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "nested module %s", name);
        module = MIR_new_module (ctx, name);
      } else if (strcmp (name, "endmodule") == 0) {
        if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "endmodule should have no labels");
        if (module == NULL)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "endmodule without module");
        MIR_finish_module (ctx);
        module = NULL;
      } else if (strcmp (name, "proto") == 0) {
        name = read_name (ctx, module, "wrong prototype name");
        if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "insn label before proto %s", name);
        if (module == NULL)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "prototype %s outside module", name);
        if (func_proto_read (ctx, module, &nres))
          MIR_new_vararg_proto_arr (ctx, name, nres, VARR_ADDR (MIR_type_t, proto_types),
                                    VARR_LENGTH (MIR_var_t, proto_vars),
                                    VARR_ADDR (MIR_var_t, proto_vars));
        else
          MIR_new_proto_arr (ctx, name, nres, VARR_ADDR (MIR_type_t, proto_types),
                             VARR_LENGTH (MIR_var_t, proto_vars),
                             VARR_ADDR (MIR_var_t, proto_vars));
      } else if (strcmp (name, "func") == 0) {
        name = read_name (ctx, module, "wrong func name");
        if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "insn label before func %s", name);
        if (func != NULL) MIR_get_error_func (ctx) (MIR_binary_io_error, "nested func %s", name);
        if (module == NULL)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "func %s outside module", name);
        if (func_proto_read (ctx, module, &nres))
          func = MIR_new_vararg_func_arr (ctx, name, nres, VARR_ADDR (MIR_type_t, proto_types),
                                          VARR_LENGTH (MIR_var_t, proto_vars),
                                          VARR_ADDR (MIR_var_t, proto_vars));
        else
          func = MIR_new_func_arr (ctx, name, nres, VARR_ADDR (MIR_type_t, proto_types),
                                   VARR_LENGTH (MIR_var_t, proto_vars),
                                   VARR_ADDR (MIR_var_t, proto_vars));
        VARR_TRUNC (MIR_label_t, func_labels, 0);
      } else if (strcmp (name, "endfunc") == 0) {
        if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "endfunc should have no labels");
        if (func == NULL) MIR_get_error_func (ctx) (MIR_binary_io_error, "endfunc without func");
        MIR_finish_func (ctx);
        func = NULL;
      } else if (strcmp (name, "export") == 0) {
        name = read_name (ctx, module, "wrong export name");
        if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "export %s should have no labels", name);
        MIR_new_export (ctx, name);
      } else if (strcmp (name, "import") == 0) {
        name = read_name (ctx, module, "wrong import name");
        if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "import %s should have no labels", name);
        MIR_new_import (ctx, name);
      } else if (strcmp (name, "forward") == 0) {
        name = read_name (ctx, module, "wrong forward name");
        if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "forward %s should have no labels", name);
        MIR_new_forward (ctx, name);
      } else if (strcmp (name, "nbss") == 0 || strcmp (name, "bss") == 0) {
        name = strcmp (name, "nbss") == 0 ? read_name (ctx, module, "wrong bss name") : NULL;
        if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "bss %s should have no labels",
                                    name == NULL ? "" : name);
        u = read_uint (ctx, "wrong bss len");
        MIR_new_bss (ctx, name, u);
      } else if (strcmp (name, "nref") == 0 || strcmp (name, "ref") == 0) {
        name = strcmp (name, "nref") == 0 ? read_name (ctx, module, "wrong ref data name") : NULL;
        if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "ref data %s should have no labels",
                                    name == NULL ? "" : name);
        item_name = read_name (ctx, module, "wrong ref data item name");
        if ((item = item_tab_find (ctx, item_name, module)) == NULL)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "ref data refers to non-existing item %s",
                                    item_name);
        i = read_int (ctx, "wrong ref disp");
        MIR_new_ref_data (ctx, name, item, i);
      } else if ((nlref_p = strcmp (name, "nlref") == 0) || strcmp (name, "lref") == 0) {
        name = NULL;
        if (nlref_p) name = read_name (ctx, module, "wrong lref data name");
        if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "lref data %s should have no labels",
                                    name == NULL ? "" : name);
        i = read_int (ctx, "wrong lref label num");
        lab = create_label (ctx, i);
        i = read_int (ctx, "wrong 2nd lref label num");
        lab2 = i < 0 ? NULL : create_label (ctx, i);
        i = read_int (ctx, "wrong lref disp");
        MIR_new_lref_data (ctx, name, lab, lab2, i);
      } else if (strcmp (name, "nexpr") == 0 || strcmp (name, "expr") == 0) {
        name = strcmp (name, "nexpr") == 0 ? read_name (ctx, module, "wrong expr name") : NULL;
        if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "expr %s should have no labels",
                                    name == NULL ? "" : name);
        item_name = read_name (ctx, module, "wrong expr func name");
        if ((item = item_tab_find (ctx, item_name, module)) == NULL
            || item->item_type != MIR_func_item)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "expr refers to non-function %s",
                                    item_name);
        MIR_new_expr_data (ctx, name, item);
      } else if (strcmp (name, "ndata") == 0 || strcmp (name, "data") == 0) {
        MIR_type_t type;
        union {
          uint8_t u8;
          uint16_t u16;
          uint32_t u32;
          uint64_t u64;
          int8_t i8;
          int16_t i16;
          int32_t i32;
          int64_t i64;
        } v;

        name = strcmp (name, "ndata") == 0 ? read_name (ctx, module, "wrong data name") : NULL;
        if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "data %s should have no labels",
                                    name == NULL ? "" : name);
        tag = read_token (ctx, &attr);
        if (TAG_TI8 > tag || tag > TAG_TRBLOCK)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "wrong data type tag %d", tag);
        type = tag_type (tag);
        VARR_TRUNC (uint8_t, temp_data, 0);
        for (;;) {
          tag = read_token (ctx, &attr);
          if (tag == TAG_EOI) break;
          switch (tag) {
          case TAG_U0:
          case TAG_U1:
          case TAG_U2:
          case TAG_U3:
          case TAG_U4:
          case TAG_U5:
          case TAG_U6:
          case TAG_U7:
          case TAG_U8:
            switch (type) {
            case MIR_T_U8:
              v.u8 = (uint8_t) attr.u;
              push_data (ctx, &v.u8, sizeof (uint8_t));
              break;
            case MIR_T_U16:
              v.u16 = (uint16_t) attr.u;
              push_data (ctx, (uint8_t *) &v.u16, sizeof (uint16_t));
              break;
            case MIR_T_U32:
              v.u32 = (uint32_t) attr.u;
              push_data (ctx, (uint8_t *) &v.u32, sizeof (uint32_t));
              break;
            case MIR_T_U64:
              v.u64 = attr.u;
              push_data (ctx, (uint8_t *) &v.i64, sizeof (uint64_t));
              break;
            default:
              MIR_get_error_func (ctx) (MIR_binary_io_error,
                                        "data type %s does not correspond value type",
                                        type_str (ctx, type));
            }
            break;
          case TAG_I1:
          case TAG_I2:
          case TAG_I3:
          case TAG_I4:
          case TAG_I5:
          case TAG_I6:
          case TAG_I7:
          case TAG_I8:
            switch (type) {
            case MIR_T_I8:
              v.i8 = (int8_t) attr.i;
              push_data (ctx, (uint8_t *) &v.i8, sizeof (int8_t));
              break;
            case MIR_T_I16:
              v.i16 = (int16_t) attr.i;
              push_data (ctx, (uint8_t *) &v.i16, sizeof (int16_t));
              break;
            case MIR_T_I32:
              v.i32 = (int32_t) attr.i;
              push_data (ctx, (uint8_t *) &v.i32, sizeof (int32_t));
              break;
            case MIR_T_I64:
              v.i64 = attr.i;
              push_data (ctx, (uint8_t *) &v.i64, sizeof (int64_t));
              break;
            default:
              MIR_get_error_func (ctx) (MIR_binary_io_error,
                                        "data type %s does not correspond value type",
                                        type_str (ctx, type));
            }
            break;
          case TAG_F:
            if (type != MIR_T_F)
              MIR_get_error_func (ctx) (MIR_binary_io_error,
                                        "data type %s does not correspond value type",
                                        type_str (ctx, type));
            push_data (ctx, (uint8_t *) &attr.f, sizeof (float));
            break;
          case TAG_D:
            if (type != MIR_T_D)
              MIR_get_error_func (ctx) (MIR_binary_io_error,
                                        "data type %s does not correspond value type",
                                        type_str (ctx, type));
            push_data (ctx, (uint8_t *) &attr.d, sizeof (double));
            break;
          case TAG_LD:
            if (type != MIR_T_LD)
              MIR_get_error_func (ctx) (MIR_binary_io_error,
                                        "data type %s does not correspond value type",
                                        type_str (ctx, type));
            push_data (ctx, (uint8_t *) &attr.ld, sizeof (long double));
            break;
            /* ??? ptr */
          default: MIR_get_error_func (ctx) (MIR_binary_io_error, "wrong data value tag %d", tag);
          }
        }
        MIR_new_data (ctx, name, type,
                      VARR_LENGTH (uint8_t, temp_data) / _MIR_type_size (ctx, type),
                      VARR_ADDR (uint8_t, temp_data));
      } else if ((global_p = strcmp (name, "global") == 0) || strcmp (name, "local") == 0) {
        if (func == NULL)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "local/global outside func");
        if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
          MIR_get_error_func (ctx) (MIR_binary_io_error, "local/global should have no labels");
        tag = read_token (ctx, &attr);
        for (;;) {
          if (tag == TAG_EOI) break;
          if (TAG_TI8 > tag || tag > TAG_TRBLOCK)
            MIR_get_error_func (ctx) (MIR_binary_io_error, "wrong local/global var type tag %d",
                                      tag);
          name = read_name (ctx, module, "wrong local/global var name");
          type_tag = tag;
          tag = read_token (ctx, &attr);
          if (!global_p) {
            MIR_new_func_reg (ctx, func->u.func, tag_type (type_tag), name);
          } else if (TAG_NAME1 <= tag && tag <= TAG_NAME4) {
            const char *reg_name = to_str (ctx, get_uint (ctx, tag - TAG_NAME1 + 1)).s;
            MIR_new_global_func_reg (ctx, func->u.func, tag_type (type_tag), name, reg_name);
            tag = read_token (ctx, &attr);
          } else {
            MIR_get_error_func (ctx) (MIR_binary_io_error, "global without hard reg name");
          }
        }
      } else {
        MIR_get_error_func (ctx) (MIR_binary_io_error, "unknown insn name %s", name);
      }
    } else if (TAG_U0 <= tag && tag <= TAG_U8) { /* insn code */
      MIR_insn_code_t insn_code = attr.u;

      if (insn_code >= MIR_LABEL)
        MIR_get_error_func (ctx) (MIR_binary_io_error, "wrong insn code %d", insn_code);
      if (insn_code == MIR_UNSPEC || insn_code == MIR_USE || insn_code == MIR_PHI)
        MIR_get_error_func (ctx) (MIR_binary_io_error,
                                  "UNSPEC, USE, or PHI is not portable and can not be read");
      for (size_t j = 0; j < VARR_LENGTH (uint64_t, insn_label_string_nums); j++) {
        lab = to_lab (ctx, VARR_GET (uint64_t, insn_label_string_nums, j));
        MIR_append_insn (ctx, func, lab);
      }
      nop = insn_code_nops (ctx, insn_code);
      mir_assert (nop != 0 || MIR_call_code_p (insn_code) || insn_code == MIR_RET
                  || insn_code == MIR_SWITCH);
      for (n = 0; (nop == 0 || n < nop) && read_operand (ctx, &op, func); n++)
        VARR_PUSH (MIR_op_t, read_insn_ops, op);
      if (nop != 0 && n < nop)
        MIR_get_error_func (ctx) (MIR_binary_io_error, "wrong number of operands of insn %s",
                                  insn_name (insn_code));
      MIR_append_insn (ctx, func,
                       MIR_new_insn_arr (ctx, insn_code, n, VARR_ADDR (MIR_op_t, read_insn_ops)));
    } else if (tag == TAG_EOFILE) {
      break;
    } else {
      MIR_get_error_func (ctx) (MIR_binary_io_error, "wrong token %d", tag);
    }
  }
  if (func != NULL)
    MIR_get_error_func (ctx) (MIR_binary_io_error, "unfinished func %s", func->u.func->name);
  if (module != NULL)
    MIR_get_error_func (ctx) (MIR_binary_io_error, "unfinished module %s", module->name);
  if (reader (ctx) != EOF)
    MIR_get_error_func (ctx) (MIR_binary_io_error, "garbage at the end of file");
#ifndef MIR_NO_BIN_COMPRESSION
  reduce_decode_finish (ctx->alloc, io_reduce_data);
#endif
}

static int file_reader (MIR_context_t ctx) { return fgetc (io_file); }

void MIR_read (MIR_context_t ctx, FILE *f) {
  io_file = f;
  MIR_read_with_func (ctx, file_reader);
}

static void io_init (MIR_context_t ctx) {
  mir_assert (TAG_LAST < 127); /* see bin_tag_t */
  if ((ctx->io_ctx = MIR_malloc (ctx->alloc, sizeof (struct io_ctx))) == NULL)
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for ctx");
  VARR_CREATE (MIR_var_t, proto_vars, ctx->alloc, 0);
  VARR_CREATE (MIR_type_t, proto_types, ctx->alloc, 0);
  VARR_CREATE (MIR_op_t, read_insn_ops, ctx->alloc, 0);
  VARR_CREATE (MIR_str_t, bin_strings, ctx->alloc, 512);
  VARR_CREATE (uint64_t, insn_label_string_nums, ctx->alloc, 64);
  VARR_CREATE (MIR_label_t, func_labels, ctx->alloc, 512);
}

static void io_finish (MIR_context_t ctx) {
  VARR_DESTROY (MIR_label_t, func_labels);
  VARR_DESTROY (uint64_t, insn_label_string_nums);
  VARR_DESTROY (MIR_str_t, bin_strings);
  VARR_DESTROY (MIR_op_t, read_insn_ops);
  VARR_DESTROY (MIR_var_t, proto_vars);
  VARR_DESTROY (MIR_type_t, proto_types);
  MIR_free (ctx->alloc, ctx->io_ctx);
  ctx->io_ctx = NULL;
}

#endif /* if !MIR_NO_IO */

/* New Page */

/* Reading MIR text file */

int _MIR_name_char_p (MIR_context_t ctx MIR_UNUSED, int ch, int first_p) {
  if (isalpha (ch) || ch == '_' || ch == '$' || ch == '%' || ch == '.') return TRUE;
  return !first_p && isdigit (ch);
}

#if !MIR_NO_SCAN

#include <stddef.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <setjmp.h>

typedef struct insn_name {
  const char *name;
  MIR_insn_code_t code;
} insn_name_t;

static int insn_name_eq (insn_name_t in1, insn_name_t in2, void *arg MIR_UNUSED) {
  return strcmp (in1.name, in2.name) == 0;
}
static htab_hash_t insn_name_hash (insn_name_t in, void *arg MIR_UNUSED) {
  return (htab_hash_t) mir_hash (in.name, strlen (in.name), 0);
}

#define TC_EL(t) TC_##t
#define REP_SEP ,
enum token_code {
  REP8 (TC_EL, INT, FLOAT, DOUBLE, LDOUBLE, NAME, STR, NL, EOFILE),
  REP5 (TC_EL, LEFT_PAR, RIGHT_PAR, COMMA, SEMICOL, COL),
};
#undef REP_SEP

typedef struct token {
  int code; /* enum token_code and EOF */
  union {
    int64_t i;
    float f;
    double d;
    long double ld;
    const char *name;
    MIR_str_t str;
  } u;
} token_t;

DEF_HTAB (insn_name_t);
typedef const char *label_name_t;
DEF_VARR (label_name_t);

typedef struct label_desc {
  int def_p;
  const char *name;
  MIR_label_t label;
} label_desc_t;

DEF_HTAB (label_desc_t);

struct scan_ctx {
  jmp_buf error_jmp_buf; /* keep it here to provide malloc alignment */
  VARR (char) * error_msg_buf;
  VARR (MIR_var_t) * scan_vars;
  VARR (MIR_type_t) * scan_types;
  VARR (MIR_op_t) * scan_insn_ops;
  size_t curr_lno;
  HTAB (insn_name_t) * insn_name_tab;
  const char *input_string;
  size_t input_string_char_num;
  VARR (label_name_t) * label_names;
  HTAB (label_desc_t) * label_desc_tab;
};

#define error_jmp_buf ctx->scan_ctx->error_jmp_buf
#define error_msg_buf ctx->scan_ctx->error_msg_buf
#define scan_vars ctx->scan_ctx->scan_vars
#define scan_types ctx->scan_ctx->scan_types
#define scan_insn_ops ctx->scan_ctx->scan_insn_ops
#define curr_lno ctx->scan_ctx->curr_lno
#define insn_name_tab ctx->scan_ctx->insn_name_tab
#define input_string ctx->scan_ctx->input_string
#define input_string_char_num ctx->scan_ctx->input_string_char_num
#define label_names ctx->scan_ctx->label_names
#define label_desc_tab ctx->scan_ctx->label_desc_tab

static void scan_error (MIR_context_t ctx, const char *format, ...) {
  char message[150];
  size_t len;
  va_list va;

  va_start (va, format);
  if (VARR_LENGTH (char, error_msg_buf) != 0) VARR_POP (char, error_msg_buf); /* remove last '\0' */
  sprintf (message, "ln %lu: ", (unsigned long) curr_lno);
  VARR_PUSH_ARR (char, error_msg_buf, message, strlen (message));
  len = vsnprintf (message, sizeof (message), format, va);
  VARR_PUSH_ARR (char, error_msg_buf, message, len);
  VARR_PUSH_ARR (char, error_msg_buf, "\n", 2); /* add '\n' and '\0' */
  va_end (va);
  longjmp (error_jmp_buf, TRUE);
}

/* Read number using GET_CHAR and UNGET_CHAR and already read
   character CH.  It should be guaranted that the input has a righ
   prefix (+|-)?[0-9].  Return base, float and double flag through
   BASE, FLOAT_P, DOUBLE_P.  Put number representation (0x or 0X
   prefix is removed) into TEMP_STRING.  */
static void scan_number (MIR_context_t ctx, int ch, int get_char (MIR_context_t),
                         void unget_char (MIR_context_t, int), int *base, int *float_p,
                         int *double_p, int *ldouble_p) {
  enum scan_number_code { NUMBER_OK, ABSENT_EXPONENT, NON_DECIMAL_FLOAT, WRONG_OCTAL_INT };
  enum scan_number_code err_code = NUMBER_OK;
  int dec_p, hex_p, hex_char_p;

  *base = 10;
  *ldouble_p = *double_p = *float_p = FALSE;
  if (ch == '+' || ch == '-') {
    VARR_PUSH (char, temp_string, ch);
    ch = get_char (ctx);
  }
  mir_assert ('0' <= ch && ch <= '9');
  if (ch == '0') {
    ch = get_char (ctx);
    if (ch != 'x' && ch != 'X') {
      *base = 8;
      unget_char (ctx, ch);
      ch = '0';
    } else {
      ch = get_char (ctx);
      *base = 16;
    }
  }
  dec_p = hex_p = FALSE;
  for (;;) {
    if (ch != '_') VARR_PUSH (char, temp_string, ch);
    ch = get_char (ctx);
    if (ch == '8' || ch == '9') dec_p = TRUE;
    hex_char_p = (('a' <= ch && ch <= 'f') || ('A' <= ch && ch <= 'F'));
    if (ch != '_' && !isdigit (ch) && (*base != 16 || !hex_char_p)) break;
    if (hex_char_p) hex_p = TRUE;
  }
  mir_assert (*base == 16 || !hex_p);
  if (ch == '.') {
    *double_p = TRUE;
    do {
      if (ch != '_') VARR_PUSH (char, temp_string, ch);
      ch = get_char (ctx);
    } while (isdigit (ch) || ch == '_');
  }
  if (ch == 'e' || ch == 'E') {
    *double_p = TRUE;
    ch = get_char (ctx);
    if (ch != '+' && ch != '-' && !isdigit (ch))
      err_code = ABSENT_EXPONENT;
    else {
      VARR_PUSH (char, temp_string, 'e');
      if (ch == '+' || ch == '-') {
        VARR_PUSH (char, temp_string, ch);
        ch = get_char (ctx);
        if (!isdigit (ch)) err_code = ABSENT_EXPONENT;
      }
      if (err_code == NUMBER_OK) do {
          if (ch != '_') VARR_PUSH (char, temp_string, ch);
          ch = get_char (ctx);
        } while (isdigit (ch) || ch == '_');
    }
  }
  if (*double_p) {
    if (*base == 16)
      err_code = NON_DECIMAL_FLOAT;
    else if (ch == 'f' || ch == 'F') {
      *float_p = TRUE;
      *double_p = FALSE;
      ch = get_char (ctx);
    } else if (ch == 'l' || ch == 'L') {
#if !defined(_WIN32) && __SIZEOF_LONG_DOUBLE__ != 8
      *ldouble_p = TRUE;
      *double_p = FALSE;
#endif
      ch = get_char (ctx);
    }
  } else if (*base == 8 && dec_p)
    err_code = WRONG_OCTAL_INT;
  VARR_PUSH (char, temp_string, '\0');
  unget_char (ctx, ch);
}

static void scan_string (MIR_context_t ctx, token_t *t, int c, int get_char (MIR_context_t),
                         void unget_char (MIR_context_t, int)) {
  int ch_code;

  mir_assert (c == '\"');
  VARR_TRUNC (char, temp_string, 0);
  for (;;) {
    if ((c = get_char (ctx)) == EOF || c == '\n') {
      VARR_PUSH (char, temp_string, '\0');
      scan_error (ctx, "unfinished string \"%s", VARR_ADDR (char, temp_string));
    }
    if (c == '"') break;
    if (c == '\\') {
      if ((c = get_char (ctx)) == 'n')
        c = '\n';
      else if (c == 't')
        c = '\t';
      else if (c == 'v')
        c = '\v';
      else if (c == 'a')
        c = '\a';
      else if (c == 'b')
        c = '\b';
      else if (c == 'r')
        c = '\r';
      else if (c == 'f')
        c = '\f';
      else if (c == '\\' || c == '\'' || c == '\"')
        ;
      else if (c == '\n') {
        curr_lno++;
        continue;
      } else if (isdigit (c) && c != '8' && c != '9') {
        ch_code = c - '0';
        c = get_char (ctx);
        if (!isdigit (c) || c == '8' || c == '9')
          unget_char (ctx, c);
        else {
          ch_code = ch_code * 8 + c - '0';
          c = get_char (ctx);
          if (!isdigit (c) || c == '8' || c == '9')
            unget_char (ctx, c);
          else
            ch_code = ch_code * 8 + c - '0';
        }
        c = ch_code;
      } else if (c == 'x') {
        /* Hex escape code.  */
        ch_code = 0;
        for (int i = 2; i > 0; i--) {
          c = get_char (ctx);
          if (!isxdigit (c)) {
            VARR_PUSH (char, temp_string, '\0');
            scan_error (ctx, "wrong hexadecimal escape in %s", VARR_ADDR (char, temp_string));
          }
          c = '0' <= c && c <= '9' ? c - '0' : 'a' <= c && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10;
          ch_code = (ch_code << 4) | c;
        }
        c = ch_code;
      }
    }
    VARR_PUSH (char, temp_string, c);
  }
  if (VARR_LENGTH (char, temp_string) > 0 && VARR_LAST (char, temp_string) != 0)
    VARR_PUSH (char, temp_string, 0);
  t->code = TC_STR;
  t->u.str
    = string_store (ctx, &strings, &string_tab,
                    (MIR_str_t){VARR_LENGTH (char, temp_string), VARR_ADDR (char, temp_string)})
        .str;
}

static int get_string_char (MIR_context_t ctx) {
  int ch = input_string[input_string_char_num];

  if (ch == '\0') return EOF;
  input_string_char_num++;
  if (ch == '\n') curr_lno++;
  return ch;
}

static void unget_string_char (MIR_context_t ctx, int ch) {
  if (input_string_char_num == 0 || ch == EOF) return;
  input_string_char_num--;
  mir_assert (input_string[input_string_char_num] == ch);
  if (ch == '\n') curr_lno--;
}

static void scan_token (MIR_context_t ctx, token_t *token, int (*get_char) (MIR_context_t),
                        void (*unget_char) (MIR_context_t, int)) {
  int ch;

  for (;;) {
    ch = get_char (ctx);
    switch (ch) {
    case EOF: token->code = TC_EOFILE; return;
    case ' ':
    case '\t': break;
    case '#':
      while ((ch = get_char (ctx)) != '\n' && ch != EOF)
        ;
      /* falls through */
    case '\n': token->code = TC_NL; return;
    case '(': token->code = TC_LEFT_PAR; return;
    case ')': token->code = TC_RIGHT_PAR; return;
    case ',': token->code = TC_COMMA; return;
    case ';': token->code = TC_SEMICOL; return;
    case ':': token->code = TC_COL; return;
    case '"': scan_string (ctx, token, ch, get_char, unget_char); return;
    default:
      VARR_TRUNC (char, temp_string, 0);
      if (_MIR_name_char_p (ctx, ch, TRUE)) {
        do {
          VARR_PUSH (char, temp_string, ch);
          ch = get_char (ctx);
        } while (_MIR_name_char_p (ctx, ch, FALSE));
        VARR_PUSH (char, temp_string, '\0');
        unget_char (ctx, ch);
        token->u.name = _MIR_uniq_string (ctx, VARR_ADDR (char, temp_string));
        token->code = TC_NAME;
        return;
      } else if (ch == '+' || ch == '-' || isdigit (ch)) {
        const char *repr;
        char *end;
        int next_ch, base, float_p, double_p, ldouble_p;

        if (ch == '+' || ch == '-') {
          next_ch = get_char (ctx);
          if (!isdigit (next_ch)) scan_error (ctx, "no number after a sign %c", ch);
          unget_char (ctx, next_ch);
        }
        scan_number (ctx, ch, get_char, unget_char, &base, &float_p, &double_p, &ldouble_p);
        repr = VARR_ADDR (char, temp_string);
        errno = 0;
        if (float_p) {
          token->code = TC_FLOAT;
          token->u.f = strtof (repr, &end);
        } else if (double_p) {
          token->code = TC_DOUBLE;
          token->u.d = strtod (repr, &end);
        } else if (ldouble_p) {
          token->code = TC_LDOUBLE;
          token->u.ld = strtold (repr, &end);
        } else {
          token->code = TC_INT;
          token->u.i = (sizeof (long) == sizeof (int64_t) ? strtoul (repr, &end, base)
                                                          : strtoull (repr, &end, base));
        }
        mir_assert (*end == '\0');
        if (errno != 0) {
        }
        return;
      } else {
        VARR_PUSH (char, temp_string, '\0');
        scan_error (ctx, "wrong char after %s", VARR_ADDR (char, temp_string));
      }
    }
  }
}

static int label_eq (label_desc_t l1, label_desc_t l2, void *arg MIR_UNUSED) {
  return strcmp (l1.name, l2.name) == 0;
}
static htab_hash_t label_hash (label_desc_t l, void *arg MIR_UNUSED) {
  return (htab_hash_t) mir_hash (l.name, strlen (l.name), 0);
}

static MIR_label_t create_label_desc (MIR_context_t ctx, const char *name, int def_p) {
  MIR_label_t label;
  label_desc_t label_desc;

  label_desc.name = name;
  if (HTAB_DO (label_desc_t, label_desc_tab, label_desc, HTAB_FIND, label_desc)) {
    if (def_p) {
      if (label_desc.def_p) scan_error (ctx, "redefinition of label %s in a module", name);
      label_desc.def_p = TRUE;
      HTAB_DO (label_desc_t, label_desc_tab, label_desc, HTAB_REPLACE, label_desc);
    }
    label = label_desc.label;
  } else {
    label_desc.label = label = MIR_new_label (ctx);
    label_desc.def_p = def_p;
    HTAB_DO (label_desc_t, label_desc_tab, label_desc, HTAB_INSERT, label_desc);
  }
  return label;
}

static int func_reg_p (MIR_context_t ctx MIR_UNUSED, MIR_func_t func, const char *name) {
  func_regs_t func_regs = func->internal;
  size_t rdn, tab_rdn;
  reg_desc_t rd;
  int res;

  rd.name = (char *) name;
  rdn = VARR_LENGTH (reg_desc_t, func_regs->reg_descs);
  VARR_PUSH (reg_desc_t, func_regs->reg_descs, rd);
  res = HTAB_DO (size_t, func_regs->name2rdn_tab, rdn, HTAB_FIND, tab_rdn);
  VARR_POP (reg_desc_t, func_regs->reg_descs);
  return res;
}

static void read_func_proto (MIR_context_t ctx, size_t nops, MIR_op_t *ops) {
  MIR_var_t var;
  size_t i;

  VARR_TRUNC (MIR_type_t, scan_types, 0);
  for (i = 0; i < nops; i++) {
    var.name = (const char *) ops[i].u.mem.disp;
    if ((var.name = (const char *) ops[i].u.mem.disp) != NULL) break;
    var.type = ops[i].u.mem.type;
    VARR_PUSH (MIR_type_t, scan_types, var.type);
  }
  VARR_TRUNC (MIR_var_t, scan_vars, 0);
  for (; i < nops; i++) {
    if (ops[i].mode != MIR_OP_MEM) scan_error (ctx, "wrong prototype/func arg");
    var.type = ops[i].u.mem.type;
    var.name = (const char *) ops[i].u.mem.disp;
    if (var.name == NULL)
      scan_error (ctx, "all func/prototype args should have form type:name or (r)blk:size(name)");
    if (MIR_all_blk_type_p (var.type)) var.size = ops[i].u.mem.base;
    VARR_PUSH (MIR_var_t, scan_vars, var);
  }
}

static MIR_type_t str2type (const char *type_name) {
  if (strcmp (type_name, "i64") == 0) return MIR_T_I64;
  if (strcmp (type_name, "u64") == 0) return MIR_T_U64;
  if (strcmp (type_name, "f") == 0) return MIR_T_F;
  if (strcmp (type_name, "d") == 0) return MIR_T_D;
  if (strcmp (type_name, "ld") == 0) return MIR_T_LD;
  if (strcmp (type_name, "p") == 0) return MIR_T_P;
  if (strcmp (type_name, "i32") == 0) return MIR_T_I32;
  if (strcmp (type_name, "u32") == 0) return MIR_T_U32;
  if (strcmp (type_name, "i16") == 0) return MIR_T_I16;
  if (strcmp (type_name, "u16") == 0) return MIR_T_U16;
  if (strcmp (type_name, "i8") == 0) return MIR_T_I8;
  if (strcmp (type_name, "u8") == 0) return MIR_T_U8;
  if (strncmp (type_name, "blk", 3) == 0) {
    int i, n = 0;
    for (i = 3; isdigit (type_name[i]) && n < MIR_BLK_NUM; i++) n = n * 10 + (type_name[i] - '0');
    if (type_name[i] == 0 && n < MIR_BLK_NUM) return MIR_T_BLK + n;
  }
  if (strcmp (type_name, "rblk") == 0) return MIR_T_RBLK;
  return MIR_T_BOUND;
}

/* Syntax:
     program: { insn / sep }
     sep : ';' | NL
     insn : {label ':'}* [ code [ {op / ','} ] ]
     label : name
     code : name
     op : name | int | float | double | long double | mem | str
     mem : type ':' addr aliases
     addr : disp
          | [ disp ] '(' sib ')'
     sib : name | [ name ] ',' name [ ',' scale]
     disp : int | name
     scale : int
     aliases :  [':' [name] [':' name] ]
*/

void MIR_scan_string (MIR_context_t ctx, const char *str) {
  token_t t;
  const char *name;
  MIR_module_t module = NULL;
  MIR_item_t item, func = NULL;
  MIR_insn_code_t insn_code = MIR_INSN_BOUND; /* for removing uninitialized warning */
  MIR_insn_t insn;
  MIR_type_t type, data_type = MIR_T_BOUND;
  MIR_op_t op, *op_addr;
  MIR_label_t label;
  int64_t i, n;
  int module_p, end_module_p, proto_p, func_p, end_func_p, dots_p, export_p, import_p, forward_p;
  int bss_p, ref_p, lref_p, expr_p, string_p, global_p, local_p, push_op_p, read_p, disp_p;
  insn_name_t in, el;

  VARR_TRUNC (char, error_msg_buf, 0);
  curr_lno = 1;
  input_string = str;
  input_string_char_num = 0;
  t.code = TC_NL;
  for (;;) {
    if (setjmp (error_jmp_buf)) {
      while (t.code != TC_NL && t.code != TC_EOFILE)
        scan_token (ctx, &t, get_string_char, unget_string_char);
      if (t.code == TC_EOFILE) break;
    }
    VARR_TRUNC (label_name_t, label_names, 0);
    scan_token (ctx, &t, get_string_char, unget_string_char);
    while (t.code == TC_NL) scan_token (ctx, &t, get_string_char, unget_string_char);
    if (t.code == TC_EOFILE) break;
    for (;;) { /* label_names */
      if (t.code != TC_NAME) scan_error (ctx, "insn should start with label or insn name");
      name = t.u.name;
      scan_token (ctx, &t, get_string_char, unget_string_char);
      if (t.code != TC_COL) break;
      VARR_PUSH (label_name_t, label_names, name);
      if (module != NULL)
        process_reserved_name (name, TEMP_ITEM_NAME_PREFIX, &module->last_temp_item_num);
      scan_token (ctx, &t, get_string_char, unget_string_char);
      if (t.code == TC_NL)
        scan_token (ctx, &t, get_string_char, unget_string_char); /* label_names without insn */
    }
    module_p = end_module_p = proto_p = func_p = end_func_p = FALSE;
    export_p = import_p = forward_p = bss_p = ref_p = lref_p = expr_p = string_p = FALSE;
    global_p = local_p = FALSE;
    if (strcmp (name, "module") == 0) {
      module_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) != 1)
        scan_error (ctx, "only one label should be used for module");
    } else if (strcmp (name, "endmodule") == 0) {
      end_module_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) != 0)
        scan_error (ctx, "endmodule should have no labels");
    } else if (strcmp (name, "proto") == 0) {
      proto_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) != 1)
        scan_error (ctx, "only one label should be used for proto");
    } else if (strcmp (name, "func") == 0) {
      func_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) != 1)
        scan_error (ctx, "only one label should be used for func");
    } else if (strcmp (name, "endfunc") == 0) {
      end_func_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) != 0)
        scan_error (ctx, "endfunc should have no labels");
    } else if (strcmp (name, "export") == 0) {
      export_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) != 0)
        scan_error (ctx, "export should have no labels");
    } else if (strcmp (name, "import") == 0) {
      import_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) != 0)
        scan_error (ctx, "import should have no labels");
    } else if (strcmp (name, "forward") == 0) {
      forward_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) != 0)
        scan_error (ctx, "forward should have no labels");
    } else if (strcmp (name, "bss") == 0) {
      bss_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) > 1)
        scan_error (ctx, "at most one label should be used for bss");
    } else if (strcmp (name, "ref") == 0) {
      ref_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) > 1)
        scan_error (ctx, "at most one label should be used for ref");
    } else if (strcmp (name, "lref") == 0) {
      lref_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) > 1)
        scan_error (ctx, "at most one label should be used for lref");
    } else if (strcmp (name, "expr") == 0) {
      expr_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) > 1)
        scan_error (ctx, "at most one label should be used for expr");
    } else if (strcmp (name, "string") == 0) {
      string_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) > 1)
        scan_error (ctx, "at most one label should be used for string");
    } else if ((local_p = strcmp (name, "local") == 0)
               || (global_p = strcmp (name, "global") == 0)) {
      if (func == NULL) scan_error (ctx, "local/global outside func");
      if (VARR_LENGTH (label_name_t, label_names) != 0)
        scan_error (ctx, "local/global should have no labels");
    } else if ((data_type = str2type (name)) != MIR_T_BOUND) {
      if (VARR_LENGTH (label_name_t, label_names) > 1)
        scan_error (ctx, "at most one label should be used for data");
    } else {
      in.name = name;
      if (!HTAB_DO (insn_name_t, insn_name_tab, in, HTAB_FIND, el))
        scan_error (ctx, "Unknown insn %s", name);
      insn_code = el.code;
      if (insn_code == MIR_UNSPEC || insn_code == MIR_USE || insn_code == MIR_PHI)
        scan_error (ctx, "UNSPEC, USE, or PHI is not portable and can not be scanned", name);
      for (n = 0; n < (int64_t) VARR_LENGTH (label_name_t, label_names); n++) {
        label = create_label_desc (ctx, VARR_GET (label_name_t, label_names, n), TRUE);
        if (func != NULL) MIR_append_insn (ctx, func, label);
      }
    }
    VARR_TRUNC (MIR_op_t, scan_insn_ops, 0);
    dots_p = FALSE;
    for (;;) { /* ops */
      if (t.code == TC_NL || t.code == TC_SEMICOL) {
        /* insn end */
        break;
      }
      push_op_p = read_p = TRUE;
      switch (t.code) {
      case TC_NAME: {
        name = t.u.name;
        scan_token (ctx, &t, get_string_char, unget_string_char);
        if ((func_p || proto_p) && strcmp (name, "...") == 0) {
          dots_p = TRUE;
          break;
        }
        read_p = FALSE;
        if (t.code != TC_COL && !proto_p && !func_p && !local_p && !global_p) {
          if (export_p) {
            MIR_new_export (ctx, name);
            push_op_p = FALSE;
          } else if (import_p) {
            MIR_new_import (ctx, name);
            push_op_p = FALSE;
          } else if (forward_p) {
            MIR_new_forward (ctx, name);
            push_op_p = FALSE;
          } else if (lref_p) {
            op = MIR_new_label_op (ctx, create_label_desc (ctx, name, FALSE));
          } else if (!module_p && !end_module_p && !end_func_p
                     && (((MIR_branch_code_p (insn_code) || insn_code == MIR_PRBEQ
                           || insn_code == MIR_PRBNE)
                          && VARR_LENGTH (MIR_op_t, scan_insn_ops) == 0)
                         || (insn_code == MIR_LADDR && VARR_LENGTH (MIR_op_t, scan_insn_ops) == 1)
                         || (insn_code == MIR_SWITCH
                             && VARR_LENGTH (MIR_op_t, scan_insn_ops) > 0))) {
            op = MIR_new_label_op (ctx, create_label_desc (ctx, name, FALSE));
          } else if (!expr_p && !ref_p && func != NULL && func_reg_p (ctx, func->u.func, name)) {
            op.mode = MIR_OP_REG;
            op.u.reg = MIR_reg (ctx, name, func->u.func);
          } else if ((item = item_tab_find (ctx, name, module)) != NULL) {
            op = MIR_new_ref_op (ctx, item);
          } else {
            scan_error (ctx, "undeclared name %s", name);
          }
          break;
        } /* Memory, type only, arg, or var */
        type = str2type (name);
        if (type == MIR_T_BOUND)
          scan_error (ctx, "Unknown type %s", name);
        else if ((global_p || local_p) && type != MIR_T_I64 && type != MIR_T_F && type != MIR_T_D
                 && type != MIR_T_LD)
          scan_error (ctx, "wrong type %s for local/global var", name);
        op = MIR_new_mem_op (ctx, type, 0, 0, 0, 1);
        if (proto_p || func_p || global_p || local_p) {
          if (t.code == TC_COL) {
            scan_token (ctx, &t, get_string_char, unget_string_char);
            if (t.code == TC_NAME) {
              op.u.mem.disp = (MIR_disp_t) t.u.name;
              scan_token (ctx, &t, get_string_char, unget_string_char);
              if (!global_p) {
              } else if (t.code != TC_COL) {
                scan_error (ctx, "global %s without hard register", (const char *) op.u.mem.disp);
              } else {
                scan_token (ctx, &t, get_string_char, unget_string_char);
                if (t.code != TC_NAME) {
                  scan_error (ctx, "hard register for %s is not a name", (char *) op.data);
                } else {
                  op.data = (void *) t.u.name;
                  scan_token (ctx, &t, get_string_char, unget_string_char);
                }
              }
            } else if (global_p || local_p || t.code != TC_INT || !MIR_all_blk_type_p (type)) {
              scan_error (ctx, local_p ? "wrong var" : "wrong arg");
              scan_token (ctx, &t, get_string_char, unget_string_char);
            } else {
              op.u.mem.base = (MIR_reg_t) t.u.i;
              if (t.u.i < 0 || t.u.i >= (1ll << sizeof (MIR_reg_t) * 8))
                scan_error (ctx, "invalid block arg size");
              scan_token (ctx, &t, get_string_char, unget_string_char);
              if (t.code != TC_LEFT_PAR) scan_error (ctx, "wrong block arg");
              scan_token (ctx, &t, get_string_char, unget_string_char);
              if (t.code != TC_NAME) scan_error (ctx, "wrong block arg");
              op.u.mem.disp = (MIR_disp_t) t.u.name;
              scan_token (ctx, &t, get_string_char, unget_string_char);
              if (t.code != TC_RIGHT_PAR) scan_error (ctx, "wrong block arg");
              scan_token (ctx, &t, get_string_char, unget_string_char);
            }
          }
        } else {
          scan_token (ctx, &t, get_string_char, unget_string_char);
          disp_p = FALSE;
          if (t.code == TC_INT) {
            op.u.mem.disp = t.u.i;
            scan_token (ctx, &t, get_string_char, unget_string_char);
            disp_p = TRUE;
          } else if (t.code == TC_NAME) {
            op.u.mem.disp = (MIR_disp_t) t.u.name;
            scan_token (ctx, &t, get_string_char, unget_string_char);
            disp_p = TRUE;
          }
          if (t.code == TC_LEFT_PAR) {
            scan_token (ctx, &t, get_string_char, unget_string_char);
            if (t.code == TC_NAME) {
              op.u.mem.base = MIR_reg (ctx, t.u.name, func->u.func);
              scan_token (ctx, &t, get_string_char, unget_string_char);
            }
            if (t.code == TC_COMMA) {
              scan_token (ctx, &t, get_string_char, unget_string_char);
              if (t.code != TC_NAME) scan_error (ctx, "wrong index");
              op.u.mem.index = MIR_reg (ctx, t.u.name, func->u.func);
              scan_token (ctx, &t, get_string_char, unget_string_char);
              if (t.code == TC_COMMA) {
                scan_token (ctx, &t, get_string_char, unget_string_char);
                if (t.code != TC_INT) scan_error (ctx, "wrong scale");
                op.u.mem.scale = (MIR_scale_t) t.u.i;
                scan_token (ctx, &t, get_string_char, unget_string_char);
              }
            }
            if (t.code != TC_RIGHT_PAR) scan_error (ctx, "wrong memory op");
            scan_token (ctx, &t, get_string_char, unget_string_char);
          } else if (!disp_p) {
            scan_error (ctx, "wrong memory");
          }
          if (t.code == TC_COL) {
            scan_token (ctx, &t, get_string_char, unget_string_char);
            if (t.code == TC_COL) {
              op.u.mem.alias = 0;
              scan_token (ctx, &t, get_string_char, unget_string_char);
              if (t.code != TC_NAME) {
                scan_error (ctx, "empty nonalias name");
              } else {
                op.u.mem.nonalias = MIR_alias (ctx, t.u.name);
                scan_token (ctx, &t, get_string_char, unget_string_char);
              }
            } else if (t.code != TC_NAME) {
              scan_error (ctx, "wrong alias name");
            } else {
              op.u.mem.alias = MIR_alias (ctx, t.u.name);
              scan_token (ctx, &t, get_string_char, unget_string_char);
              if (t.code == TC_COL) {
                scan_token (ctx, &t, get_string_char, unget_string_char);
                if (t.code != TC_NAME) {
                  scan_error (ctx, "empty nonalias name");
                } else {
                  op.u.mem.nonalias = MIR_alias (ctx, t.u.name);
                  scan_token (ctx, &t, get_string_char, unget_string_char);
                }
              }
            }
          }
        }
        break;
      }
      case TC_INT:
        op.mode = MIR_OP_INT;
        op.u.i = t.u.i;
        break;
      case TC_FLOAT:
        op.mode = MIR_OP_FLOAT;
        op.u.f = t.u.f;
        break;
      case TC_LDOUBLE: op.mode = MIR_OP_LDOUBLE; op.u.ld = t.u.ld;
#if !defined(_WIN32) && __SIZEOF_LONG_DOUBLE__ != 8
        break;
#endif
      case TC_DOUBLE:
        op.mode = MIR_OP_DOUBLE;
        op.u.d = t.u.d;
        break;
      case TC_STR:
        op.mode = MIR_OP_STR;
        op.u.str = t.u.str;
        break;
      default: break;
      }
      if (dots_p) break;
      if (push_op_p) {
        VARR_PUSH (MIR_op_t, scan_insn_ops, op);
        op.data = NULL; /* reset value set up for global */
      }
      if (read_p) scan_token (ctx, &t, get_string_char, unget_string_char);
      if (t.code != TC_COMMA) break;
      scan_token (ctx, &t, get_string_char, unget_string_char);
    }
    if (t.code != TC_NL && t.code != TC_EOFILE && t.code != TC_SEMICOL)
      scan_error (ctx, "wrong insn end");
    if (module_p) {
      if (module != NULL) scan_error (ctx, "nested module");
      if (VARR_LENGTH (MIR_op_t, scan_insn_ops) != 0)
        scan_error (ctx, "module should have no params");
      module = MIR_new_module (ctx, VARR_GET (label_name_t, label_names, 0));
      HTAB_CLEAR (label_desc_t, label_desc_tab);
    } else if (end_module_p) {
      if (module == NULL) scan_error (ctx, "standalone endmodule");
      if (VARR_LENGTH (MIR_op_t, scan_insn_ops) != 0)
        scan_error (ctx, "endmodule should have no params");
      MIR_finish_module (ctx);
      module = NULL;
    } else if (bss_p) {
      if (VARR_LENGTH (MIR_op_t, scan_insn_ops) != 1)
        scan_error (ctx, "bss should have one operand");
      op_addr = VARR_ADDR (MIR_op_t, scan_insn_ops);
      if (op_addr[0].mode != MIR_OP_INT || op_addr[0].u.i < 0)
        scan_error (ctx, "wrong bss operand type or value");
      name
        = (VARR_LENGTH (label_name_t, label_names) == 0 ? NULL
                                                        : VARR_GET (label_name_t, label_names, 0));
      MIR_new_bss (ctx, name, op_addr[0].u.i);
    } else if (ref_p) {
      if (VARR_LENGTH (MIR_op_t, scan_insn_ops) != 2)
        scan_error (ctx, "ref should have two operands");
      op_addr = VARR_ADDR (MIR_op_t, scan_insn_ops);
      if (op_addr[0].mode != MIR_OP_REF) scan_error (ctx, "wrong ref operand");
      if (op_addr[1].mode != MIR_OP_INT) scan_error (ctx, "wrong ref disp operand");
      name
        = (VARR_LENGTH (label_name_t, label_names) == 0 ? NULL
                                                        : VARR_GET (label_name_t, label_names, 0));
      MIR_new_ref_data (ctx, name, op_addr[0].u.ref, op_addr[1].u.i);
    } else if (lref_p) {
      size_t len = VARR_LENGTH (MIR_op_t, scan_insn_ops);
      MIR_label_t lab = NULL, lab2 = NULL;
      int64_t disp = 0;
      if (len == 0 || len > 3)
        scan_error (ctx, "lref should have at least one but at most three operands");
      op_addr = VARR_ADDR (MIR_op_t, scan_insn_ops);
      if (op_addr[0].mode != MIR_OP_LABEL) scan_error (ctx, "1st lref operand is not a label");
      lab = op_addr[0].u.label;
      if (len == 2) {
        if (op_addr[1].mode != MIR_OP_LABEL && op_addr[1].mode != MIR_OP_INT)
          scan_error (ctx, "2nd lref operand is not a label or displacement");
        if (op_addr[1].mode == MIR_OP_LABEL) lab2 = op_addr[1].u.label;
        if (op_addr[1].mode == MIR_OP_INT) disp = op_addr[1].u.i;
      } else if (len == 3) {
        if (op_addr[1].mode != MIR_OP_LABEL) scan_error (ctx, "2nd lref operand is not a label");
        if (op_addr[2].mode != MIR_OP_INT)
          scan_error (ctx, "3rd lref operand is not a displacement");
        lab2 = op_addr[1].u.label;
        disp = op_addr[2].u.i;
      }
      name
        = (VARR_LENGTH (label_name_t, label_names) == 0 ? NULL
                                                        : VARR_GET (label_name_t, label_names, 0));
      MIR_new_lref_data (ctx, name, lab, lab2, disp);
    } else if (expr_p) {
      if (VARR_LENGTH (MIR_op_t, scan_insn_ops) != 1)
        scan_error (ctx, "expr should have one operand");
      op_addr = VARR_ADDR (MIR_op_t, scan_insn_ops);
      if (op_addr[0].mode != MIR_OP_REF || op_addr[0].u.ref->item_type != MIR_func_item)
        scan_error (ctx, "wrong expr operand");
      name
        = (VARR_LENGTH (label_name_t, label_names) == 0 ? NULL
                                                        : VARR_GET (label_name_t, label_names, 0));
      MIR_new_expr_data (ctx, name, op_addr[0].u.ref);
    } else if (string_p) {
      if (VARR_LENGTH (MIR_op_t, scan_insn_ops) != 1)
        scan_error (ctx, "string should have one operand");
      op_addr = VARR_ADDR (MIR_op_t, scan_insn_ops);
      if (op_addr[0].mode != MIR_OP_STR) scan_error (ctx, "wrong string data operand type");
      name
        = (VARR_LENGTH (label_name_t, label_names) == 0 ? NULL
                                                        : VARR_GET (label_name_t, label_names, 0));
      MIR_new_string_data (ctx, name, op_addr[0].u.str);
    } else if (proto_p) {
      if (module == NULL) scan_error (ctx, "prototype outside module");
      read_func_proto (ctx, VARR_LENGTH (MIR_op_t, scan_insn_ops),
                       VARR_ADDR (MIR_op_t, scan_insn_ops));
      if (dots_p)
        MIR_new_vararg_proto_arr (ctx, VARR_GET (label_name_t, label_names, 0),
                                  VARR_LENGTH (MIR_type_t, scan_types),
                                  VARR_ADDR (MIR_type_t, scan_types),
                                  VARR_LENGTH (MIR_var_t, scan_vars),
                                  VARR_ADDR (MIR_var_t, scan_vars));
      else
        MIR_new_proto_arr (ctx, VARR_GET (label_name_t, label_names, 0),
                           VARR_LENGTH (MIR_type_t, scan_types), VARR_ADDR (MIR_type_t, scan_types),
                           VARR_LENGTH (MIR_var_t, scan_vars), VARR_ADDR (MIR_var_t, scan_vars));
    } else if (func_p) {
      if (module == NULL) scan_error (ctx, "func outside module");
      if (func != NULL) scan_error (ctx, "nested func");
      read_func_proto (ctx, VARR_LENGTH (MIR_op_t, scan_insn_ops),
                       VARR_ADDR (MIR_op_t, scan_insn_ops));
      if (dots_p)
        func = MIR_new_vararg_func_arr (ctx, VARR_GET (label_name_t, label_names, 0),
                                        VARR_LENGTH (MIR_type_t, scan_types),
                                        VARR_ADDR (MIR_type_t, scan_types),
                                        VARR_LENGTH (MIR_var_t, scan_vars),
                                        VARR_ADDR (MIR_var_t, scan_vars));
      else
        func
          = MIR_new_func_arr (ctx, VARR_GET (label_name_t, label_names, 0),
                              VARR_LENGTH (MIR_type_t, scan_types),
                              VARR_ADDR (MIR_type_t, scan_types),
                              VARR_LENGTH (MIR_var_t, scan_vars), VARR_ADDR (MIR_var_t, scan_vars));
    } else if (end_func_p) {
      if (func == NULL) scan_error (ctx, "standalone endfunc");
      if (VARR_LENGTH (MIR_op_t, scan_insn_ops) != 0)
        scan_error (ctx, "endfunc should have no params");
      func = NULL;
      MIR_finish_func (ctx);
    } else if (export_p || import_p || forward_p) { /* we already created items, now do nothing: */
      mir_assert (VARR_LENGTH (MIR_op_t, scan_insn_ops) == 0);
    } else if (global_p || local_p) {
      op_addr = VARR_ADDR (MIR_op_t, scan_insn_ops);
      n = (int64_t) VARR_LENGTH (MIR_op_t, scan_insn_ops);
      for (i = 0; i < n; i++) {
        if (op_addr[i].mode != MIR_OP_MEM || (const char *) op_addr[i].u.mem.disp == NULL)
          scan_error (ctx, "wrong local/global var");
        if (op_addr[i].data == NULL) {
          MIR_new_func_reg (ctx, func->u.func, op_addr[i].u.mem.type,
                            (const char *) op_addr[i].u.mem.disp);
        } else {
          MIR_new_global_func_reg (ctx, func->u.func, op_addr[i].u.mem.type,
                                   (const char *) op_addr[i].u.mem.disp,
                                   (const char *) op_addr[i].data);
        }
      }
    } else if (data_type != MIR_T_BOUND) {
      union {
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        uint64_t u64;
        int8_t i8;
        int16_t i16;
        int32_t i32;
        int64_t i64;
      } v;

      n = (int64_t) VARR_LENGTH (MIR_op_t, scan_insn_ops);
      op_addr = VARR_ADDR (MIR_op_t, scan_insn_ops);
      VARR_TRUNC (uint8_t, temp_data, 0);
      for (i = 0; i < n; i++) {
        if (op_addr[i].mode != type2mode (data_type))
          scan_error (ctx, "data operand is not of data type");
        switch (data_type) {
        case MIR_T_I8:
          v.i8 = (int8_t) op_addr[i].u.i;
          push_data (ctx, (uint8_t *) &v.i8, sizeof (int8_t));
          break;
        case MIR_T_U8:
          v.u8 = (uint8_t) op_addr[i].u.u;
          push_data (ctx, (uint8_t *) &v.u8, sizeof (uint8_t));
          break;
        case MIR_T_I16:
          v.i16 = (int16_t) op_addr[i].u.i;
          push_data (ctx, (uint8_t *) &v.i16, sizeof (int16_t));
          break;
        case MIR_T_U16:
          v.u16 = (uint16_t) op_addr[i].u.u;
          push_data (ctx, (uint8_t *) &v.u16, sizeof (uint16_t));
          break;
        case MIR_T_I32:
          v.i32 = (int32_t) op_addr[i].u.i;
          push_data (ctx, (uint8_t *) &v.i32, sizeof (int32_t));
          break;
        case MIR_T_U32:
          v.u32 = (uint32_t) op_addr[i].u.u;
          push_data (ctx, (uint8_t *) &v.u32, sizeof (uint32_t));
          break;
        case MIR_T_I64:
          v.i64 = op_addr[i].u.i;
          push_data (ctx, (uint8_t *) &v.i64, sizeof (int64_t));
          break;
        case MIR_T_U64:
          v.u64 = op_addr[i].u.u;
          push_data (ctx, (uint8_t *) &v.u64, sizeof (uint64_t));
          break;
        case MIR_T_F: push_data (ctx, (uint8_t *) &op_addr[i].u.f, sizeof (float)); break;
        case MIR_T_D: push_data (ctx, (uint8_t *) &op_addr[i].u.d, sizeof (double)); break;
        case MIR_T_LD:
          push_data (ctx, (uint8_t *) &op_addr[i].u.ld, sizeof (long double));
          break;
          /* ptr ??? */
        default: scan_error (ctx, "wrong data clause");
        }
      }
      name
        = (VARR_LENGTH (label_name_t, label_names) == 0 ? NULL
                                                        : VARR_GET (label_name_t, label_names, 0));
      MIR_new_data (ctx, name, data_type,
                    VARR_LENGTH (uint8_t, temp_data) / _MIR_type_size (ctx, data_type),
                    VARR_ADDR (uint8_t, temp_data));
    } else {
      insn = MIR_new_insn_arr (ctx, insn_code, VARR_LENGTH (MIR_op_t, scan_insn_ops),
                               VARR_ADDR (MIR_op_t, scan_insn_ops));
      if (func != NULL) MIR_append_insn (ctx, func, insn);
    }
  }
  if (func != NULL) {
    if (!setjmp (error_jmp_buf)) scan_error (ctx, "absent endfunc");
  }
  if (module != NULL) {
    if (!setjmp (error_jmp_buf)) scan_error (ctx, "absent endmodule");
  }
  if (VARR_LENGTH (char, error_msg_buf) != 0)
    MIR_get_error_func (ctx) (MIR_syntax_error, VARR_ADDR (char, error_msg_buf));
}

static void scan_init (MIR_context_t ctx) {
  insn_name_t in, el;
  size_t i;

  if ((ctx->scan_ctx = MIR_malloc (ctx->alloc, sizeof (struct scan_ctx))) == NULL)
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for ctx");
  VARR_CREATE (char, error_msg_buf, ctx->alloc, 0);
  VARR_CREATE (MIR_var_t, scan_vars, ctx->alloc, 0);
  VARR_CREATE (MIR_type_t, scan_types, ctx->alloc, 0);
  VARR_CREATE (MIR_op_t, scan_insn_ops, ctx->alloc, 0);
  VARR_CREATE (label_name_t, label_names, ctx->alloc, 0);
  HTAB_CREATE (label_desc_t, label_desc_tab, ctx->alloc, 100, label_hash, label_eq, NULL);
  HTAB_CREATE (insn_name_t, insn_name_tab, ctx->alloc, MIR_INSN_BOUND, insn_name_hash, insn_name_eq, NULL);
  for (i = 0; i < MIR_INSN_BOUND; i++) {
    in.code = i;
    in.name = MIR_insn_name (ctx, i);
    HTAB_DO (insn_name_t, insn_name_tab, in, HTAB_INSERT, el);
  }
}

static void scan_finish (MIR_context_t ctx) {
  VARR_DESTROY (char, error_msg_buf);
  VARR_DESTROY (MIR_var_t, scan_vars);
  VARR_DESTROY (MIR_type_t, scan_types);
  VARR_DESTROY (MIR_op_t, scan_insn_ops);
  VARR_DESTROY (label_name_t, label_names);
  HTAB_DESTROY (label_desc_t, label_desc_tab);
  HTAB_DESTROY (insn_name_t, insn_name_tab);
  MIR_free (ctx->alloc, ctx->scan_ctx);
  ctx->scan_ctx = NULL;
}

#endif /* if !MIR_NO_SCAN */

/* New Page */

#ifndef _WIN32
#include <sys/types.h>
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define getpid GetCurrentProcessId
#define popen _popen
#define pclose _pclose
#endif

void _MIR_dump_code (const char *name, uint8_t *code, size_t code_len) {
  size_t i;
  int ch;
  char cfname[50];
  char command[500];
  FILE *f;
#if !defined(__APPLE__)
  char bfname[30];
  FILE *bf;
#endif

  if (name != NULL) fprintf (stderr, "%s:", name);
  sprintf (cfname, "_mir_%lu.c", (unsigned long) getpid ());
  if ((f = fopen (cfname, "w")) == NULL) return;
#if defined(__APPLE__)
  fprintf (f, "unsigned char code[] = {");
  for (i = 0; i < code_len; i++) {
    if (i != 0) fprintf (f, ", ");
    fprintf (f, "0x%x", code[i]);
  }
  fprintf (f, "};\n");
  fclose (f);
#if defined(__aarch64__)
  sprintf (command, "gcc -c -o %s.o %s 2>&1 && objdump --section=__data -D %s.o; rm -f %s.o %s",
           cfname, cfname, cfname, cfname, cfname);
#else
  sprintf (command, "gcc -c -o %s.o %s 2>&1 && objdump --section=.data -D %s.o; rm -f %s.o %s",
           cfname, cfname, cfname, cfname, cfname);
#endif
#else
  sprintf (bfname, "_mir_%lu.bin", (unsigned long) getpid ());
  if ((bf = fopen (bfname, "wb")) == NULL) return;
  fprintf (f, "void code (void) {}\n");
  for (i = 0; i < code_len; i++) fputc (code[i], bf);
  fclose (f);
  fclose (bf);
  sprintf (command,
           "gcc -c -o %s.o %s 2>&1 && objcopy --update-section .text=%s %s.o && objdump "
           "--adjust-vma=0x%llx -d %s.o; rm -f "
           "%s.o %s %s",
           cfname, cfname, bfname, cfname, (unsigned long long) code, cfname, cfname, cfname,
           bfname);
#endif
  fprintf (stderr, "%s\n", command);
  if ((f = popen (command, "r")) == NULL) return;
  while ((ch = fgetc (f)) != EOF) fprintf (stderr, "%c", ch);
  pclose (f);
}

/* New Page */

#if defined(__x86_64__) || defined(_M_AMD64)
#include "mir-x86_64.c"
#elif defined(__aarch64__)
#include "mir-aarch64.c"
#elif defined(__PPC64__)
#include "mir-ppc64.c"
#elif defined(__s390x__)
#include "mir-s390x.c"
#elif defined(__riscv)
#if __riscv_xlen != 64 || __riscv_flen < 64 || !__riscv_float_abi_double || !__riscv_mul \
  || !__riscv_div || !__riscv_compressed
#error "only 64-bit RISCV supported (at least rv64imafdc)"
#endif
#if __riscv_flen == 128
#error "RISCV 128-bit floats (Q set) is not supported"
#endif
#include "mir-riscv64.c"
#else
#error "undefined or unsupported generation target"
#endif

static int var_is_reg_p (MIR_reg_t var) { return var > MAX_HARD_REG; }
static MIR_reg_t var2reg (MIR_reg_t var) {
  mir_assert (var_is_reg_p (var));
  return var == MIR_NON_VAR ? 0 : var - MAX_HARD_REG;
}

struct hard_reg_desc {
  const char *name;
  int num;
};
typedef struct hard_reg_desc hard_reg_desc_t;

DEF_HTAB (hard_reg_desc_t);

struct hard_reg_ctx {
  HTAB (hard_reg_desc_t) * hard_reg_desc_tab;
};

#define hard_reg_desc_tab ctx->hard_reg_ctx->hard_reg_desc_tab

static htab_hash_t hard_reg_desc_hash (hard_reg_desc_t desc, void *arg MIR_UNUSED) {
  return (htab_hash_t) mir_hash (desc.name, strlen (desc.name), 0);
}
static int hard_reg_desc_eq (hard_reg_desc_t desc1, hard_reg_desc_t desc2, void *arg MIR_UNUSED) {
  return strcmp (desc1.name, desc2.name) == 0;
}

static void hard_reg_name_init (MIR_context_t ctx) {
  hard_reg_desc_t desc, tab_desc;
  int res;

  if ((ctx->hard_reg_ctx = MIR_malloc (ctx->alloc, sizeof (struct hard_reg_ctx))) == NULL)
    MIR_get_error_func (ctx) (MIR_alloc_error, "Not enough memory for ctx");
  HTAB_CREATE (hard_reg_desc_t, hard_reg_desc_tab, ctx->alloc, 200, hard_reg_desc_hash, hard_reg_desc_eq, NULL);
  for (size_t i = 0; i * sizeof (char *) < sizeof (target_hard_reg_names); i++) {
    desc.num = (int) i;
    desc.name = target_hard_reg_names[i];
    res = HTAB_DO (hard_reg_desc_t, hard_reg_desc_tab, desc, HTAB_INSERT, tab_desc);
    mir_assert (!res);
  }
}

static void hard_reg_name_finish (MIR_context_t ctx) {
  HTAB_DESTROY (hard_reg_desc_t, hard_reg_desc_tab);
  MIR_free (ctx->alloc, ctx->hard_reg_ctx);
  ctx->hard_reg_ctx = NULL;
}

int _MIR_get_hard_reg (MIR_context_t ctx, const char *hard_reg_name) {
  hard_reg_desc_t desc, tab_desc;

  desc.name = hard_reg_name;
  if (!HTAB_DO (hard_reg_desc_t, hard_reg_desc_tab, desc, HTAB_FIND, tab_desc)) return -1;
  return tab_desc.num;
}

static MIR_UNUSED const char *get_hard_reg_name (MIR_context_t ctx MIR_UNUSED, int hard_reg) {
  if (hard_reg > MAX_HARD_REG || target_fixed_hard_reg_p (hard_reg)) return NULL;
  return target_hard_reg_names[hard_reg];
}

void *_MIR_get_module_global_var_hard_regs (MIR_context_t ctx MIR_UNUSED, MIR_module_t module) {
  return module->data;
}

/* New Page */

#include "mir-interp.c"

/* Local Variables:                */
/* mode: c                         */
/* page-delimiter: "/\\* New Page" */
/* End:                            */
