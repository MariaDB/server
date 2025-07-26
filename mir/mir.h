/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#ifndef MIR_H

#define MIR_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && !defined(_WIN64)
#error "MIR does not work on 32-bit Windows"
#endif

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include "mir-dlist.h"
#include "mir-varr.h"
#include "mir-htab.h"
#include "mir-alloc.h"
#include "mir-code-alloc.h"

#define MIR_API_VERSION 0.2

#ifdef NDEBUG
static inline int mir_assert (int cond) { return 0 && cond; }
#else
#define mir_assert(cond) assert (cond)
#endif

#define FALSE 0
#define TRUE 1

/* Redefine MIR_NO_IO or/and MIR_NO_SCAN if you don't need the functionality they provide.  */
#ifndef MIR_NO_IO
#define MIR_NO_IO 0
#endif

#ifndef MIR_NO_SCAN
#define MIR_NO_SCAN 0
#endif

#ifdef __GNUC__
#define MIR_UNUSED __attribute__ ((unused))
#else
#define MIR_UNUSED
#endif

#define REP2(M, a1, a2) M (a1) REP_SEP M (a2)
#define REP3(M, a1, a2, a3) REP2 (M, a1, a2) REP_SEP M (a3)
#define REP4(M, a1, a2, a3, a4) REP3 (M, a1, a2, a3) REP_SEP M (a4)
#define REP5(M, a1, a2, a3, a4, a5) REP4 (M, a1, a2, a3, a4) REP_SEP M (a5)
#define REP6(M, a1, a2, a3, a4, a5, a6) REP5 (M, a1, a2, a3, a4, a5) REP_SEP M (a6)
#define REP7(M, a1, a2, a3, a4, a5, a6, a7) REP6 (M, a1, a2, a3, a4, a5, a6) REP_SEP M (a7)
#define REP8(M, a1, a2, a3, a4, a5, a6, a7, a8) REP7 (M, a1, a2, a3, a4, a5, a6, a7) REP_SEP M (a8)

#define REP_SEP ,

#define ERR_EL(e) MIR_##e##_error
typedef enum MIR_error_type {
  REP8 (ERR_EL, no, syntax, binary_io, alloc, finish, no_module, nested_module, no_func),
  REP5 (ERR_EL, func, vararg_func, nested_func, wrong_param_value, hard_reg),
  REP5 (ERR_EL, reserved_name, import_export, undeclared_func_reg, repeated_decl, reg_type),
  REP6 (ERR_EL, wrong_type, unique_reg, undeclared_op_ref, ops_num, call_op, unspec_op),
  REP6 (ERR_EL, wrong_lref, ret, op_mode, out_op, invalid_insn, ctx_change)
} MIR_error_type_t;

#ifdef __GNUC__
#define MIR_NO_RETURN __attribute__ ((noreturn))
#else
#define MIR_NO_RETURN
#endif

typedef void MIR_NO_RETURN (*MIR_error_func_t) (MIR_error_type_t error_type, const char *format,
                                                ...);

#define INSN_EL(i) MIR_##i

/* The most MIR insns have destination operand and one or two source
   operands.  The destination can be only a register or memory.

   There are additional constraints on insn operands:

   o A register in program can contain only one type values: integer,
     float, double, or long double.
   o Operand types should be what the insn expects */
typedef enum {
  /* Abbreviations:
     I - 64-bit int, S - short (32-bit), U - unsigned, F -float, D - double, LD - long double.  */
  /* 2 operand insns: */
  REP4 (INSN_EL, MOV, FMOV, DMOV, LDMOV), /* Moves */
  /* Extensions.  Truncation is not necessary because we can use an extension to use a part. */
  REP6 (INSN_EL, EXT8, EXT16, EXT32, UEXT8, UEXT16, UEXT32),
  REP3 (INSN_EL, I2F, I2D, I2LD),    /* Integer to float or (long) double conversion */
  REP3 (INSN_EL, UI2F, UI2D, UI2LD), /* Unsigned integer to float or (long) double conversion */
  REP3 (INSN_EL, F2I, D2I, LD2I),    /* Float or (long) double to integer conversion */
  REP6 (INSN_EL, F2D, F2LD, D2F, D2LD, LD2F, LD2D), /* Float, (long) double conversions */
  REP5 (INSN_EL, NEG, NEGS, FNEG, DNEG, LDNEG),     /* Changing sign */
  REP4 (INSN_EL, ADDR, ADDR8, ADDR16, ADDR32), /* reg addr in natural mode or given integer mode */
  /* 3 operand insn: */
  REP5 (INSN_EL, ADD, ADDS, FADD, DADD, LDADD),              /* Addition */
  REP5 (INSN_EL, SUB, SUBS, FSUB, DSUB, LDSUB),              /* Subtraction */
  REP5 (INSN_EL, MUL, MULS, FMUL, DMUL, LDMUL),              /* Multiplication */
  REP7 (INSN_EL, DIV, DIVS, UDIV, UDIVS, FDIV, DDIV, LDDIV), /* Division */
  REP4 (INSN_EL, MOD, MODS, UMOD, UMODS),                    /* Modulo */
  REP6 (INSN_EL, AND, ANDS, OR, ORS, XOR, XORS),             /* Logical */
  REP6 (INSN_EL, LSH, LSHS, RSH, RSHS, URSH, URSHS),         /* Right signed/unsigned shift */
  REP5 (INSN_EL, EQ, EQS, FEQ, DEQ, LDEQ),                   /* Equality */
  REP5 (INSN_EL, NE, NES, FNE, DNE, LDNE),                   /* Inequality */
  REP7 (INSN_EL, LT, LTS, ULT, ULTS, FLT, DLT, LDLT),        /* Less then */
  REP7 (INSN_EL, LE, LES, ULE, ULES, FLE, DLE, LDLE),        /* Less or equal */
  REP7 (INSN_EL, GT, GTS, UGT, UGTS, FGT, DGT, LDGT),        /* Greater then */
  REP7 (INSN_EL, GE, GES, UGE, UGES, FGE, DGE, LDGE),        /* Greater or equal */
  REP8 (INSN_EL, ADDO, ADDOS, SUBO, SUBOS, MULO, MULOS, UMULO, UMULOS), /* setting overflow flag */
  /* Unconditional (1 operand) and conditional (2 operands) branch
     insns.  The first operand is a label.  */
  REP5 (INSN_EL, JMP, BT, BTS, BF, BFS),
  /* Compare and branch (3 operand) insns.  The first operand is the
     label. */
  REP5 (INSN_EL, BEQ, BEQS, FBEQ, DBEQ, LDBEQ),
  REP5 (INSN_EL, BNE, BNES, FBNE, DBNE, LDBNE),
  REP7 (INSN_EL, BLT, BLTS, UBLT, UBLTS, FBLT, DBLT, LDBLT),
  REP7 (INSN_EL, BLE, BLES, UBLE, UBLES, FBLE, DBLE, LDBLE),
  REP7 (INSN_EL, BGT, BGTS, UBGT, UBGTS, FBGT, DBGT, LDBGT),
  REP7 (INSN_EL, BGE, BGES, UBGE, UBGES, FBGE, DBGE, LDBGE),
  REP2 (INSN_EL, BO, UBO),   /* branch on overflow: prev insn should be overflow add/sub */
  REP2 (INSN_EL, BNO, UBNO), /* branch on not overflow: prev insn should be overflow add/sub */
  INSN_EL (LADDR),           /* put label address (2nd op) into the 1st op */
  INSN_EL (JMPI),            /* indirect jump to the label whose address stored in the 1st op */
  /* 1st operand is a prototype, 2nd one is ref or op containing func
     address, 3rd and subsequent ops are optional result (if result in
     the prototype is not of void type), call arguments. */
  REP3 (INSN_EL, CALL, INLINE, JCALL),
  /* 1st operand is an index, subsequent ops are labels to which goto
     according the index (1st label has index zero).  The insn
     behavior is undefined if there is no label for the index. */
  INSN_EL (SWITCH),
  INSN_EL (RET),
  INSN_EL (JRET), /* return by jumping to address of the operand */
  /* 1 operand insn: */
  INSN_EL (ALLOCA),             /* 2 operands: result address and size  */
  REP2 (INSN_EL, BSTART, BEND), /* block start: result addr; block end: addr from block start */
  /* Special insns: */
  INSN_EL (VA_ARG),       /* result is arg address, operands: va_list addr and memory */
  INSN_EL (VA_BLOCK_ARG), /* result is arg address, operands: va_list addr, integer (size), and
                             integer (block type) */
  INSN_EL (VA_START),
  INSN_EL (VA_END),                    /* operand is va_list */
  INSN_EL (LABEL),                     /* One immediate operand is unique label number  */
  INSN_EL (UNSPEC),                    /* First operand unspec code and the rest are args */
  REP3 (INSN_EL, PRSET, PRBEQ, PRBNE), /* work with properties */
  INSN_EL (USE), /* Used only internally in the generator, all operands are input */
  INSN_EL (PHI), /* Used only internally in the generator, the first operand is output */
  INSN_EL (INVALID_INSN),
  INSN_EL (INSN_BOUND), /* Should be the last  */
} MIR_insn_code_t;

#define TYPE_EL(t) MIR_T_##t

#define MIR_BLK_NUM 5
/* Data types: */
typedef enum {
  REP8 (TYPE_EL, I8, U8, I16, U16, I32, U32, I64, U64), /* Integer types of different size: */
  REP3 (TYPE_EL, F, D, LD),                             /* Float or (long) double type */
  REP2 (TYPE_EL, P, BLK),                               /* Pointer, memory blocks */
  TYPE_EL (RBLK) = TYPE_EL (BLK) + MIR_BLK_NUM,         /* return block */
  REP2 (TYPE_EL, UNDEF, BOUND),
} MIR_type_t;

static inline int MIR_int_type_p (MIR_type_t t) {
  return (MIR_T_I8 <= t && t <= MIR_T_U64) || t == MIR_T_P;
}

static inline int MIR_fp_type_p (MIR_type_t t) { return MIR_T_F <= t && t <= MIR_T_LD; }

static inline int MIR_blk_type_p (MIR_type_t t) { return MIR_T_BLK <= t && t < MIR_T_RBLK; }
static inline int MIR_all_blk_type_p (MIR_type_t t) { return MIR_T_BLK <= t && t <= MIR_T_RBLK; }

#if UINTPTR_MAX == 0xffffffff
#define MIR_PTR32 1
#define MIR_PTR64 0
#elif UINTPTR_MAX == 0xffffffffffffffffu
#define MIR_PTR32 0
#define MIR_PTR64 1
#else
#error MIR can work only for 32- or 64-bit targets
#endif

typedef uint8_t MIR_scale_t; /* Index reg scale in memory */

#define MIR_MAX_SCALE UINT8_MAX

typedef int64_t MIR_disp_t; /* Address displacement in memory */

/* Register number (> 0).  A register always contain only one type
   value: integer, float, or (long) double.  Register numbers in insn
   operands can be changed in MIR_finish_func.  */
typedef uint32_t MIR_reg_t;

#define MIR_MAX_REG_NUM UINT32_MAX
#define MIR_NON_VAR MIR_MAX_REG_NUM

/* Immediate in immediate moves.  */
typedef union {
  int64_t i;
  uint64_t u;
  float f;
  double d;
  long double ld;
} MIR_imm_t;

typedef uint32_t MIR_alias_t; /* unique number of alias name */

/* Memory: mem:type[base + index * scale + disp].  It also can be memory with vars
   (regs and hard regs) but such memory used only internally.  An integer type memory
   value expands to int64_t value when the insn is executed.  */
typedef struct {
  MIR_type_t type : 8;
  MIR_scale_t scale;
  MIR_alias_t alias;    /* 0 may alias any memory, memory with the same alias is aliased */
  MIR_alias_t nonalias; /* 0 for ignoring, memory with the same nonalias is not aliased */
  /* Used internally: mem operand with the same nonzero nloc always refers to the same memory */
  uint32_t nloc;
  /* 0 and MIR_NON_VAR means no reg for correspondingly for memory and var memory. */
  MIR_reg_t base, index;
  MIR_disp_t disp;
} MIR_mem_t;

typedef struct MIR_insn *MIR_label_t;

typedef const char *MIR_name_t;

#define OP_EL(op) MIR_OP_##op

/* Operand mode */
typedef enum {
  REP8 (OP_EL, UNDEF, REG, VAR, INT, UINT, FLOAT, DOUBLE, LDOUBLE),
  REP6 (OP_EL, REF, STR, MEM, VAR_MEM, LABEL, BOUND),
} MIR_op_mode_t;

typedef struct MIR_item *MIR_item_t;

struct MIR_str {
  size_t len;
  const char *s;
};

typedef struct MIR_str MIR_str_t;

/* An insn operand */
typedef struct {
  void *data; /* Aux data  */
  MIR_op_mode_t mode : 8;
  /* Defined after MIR_func_finish.  Only MIR_OP_INT, MIR_OP_UINT,
     MIR_OP_FLOAT, MIR_OP_DOUBLE, MIR_OP_LDOUBLE: */
  MIR_op_mode_t value_mode : 8;
  union {
    MIR_reg_t reg;
    MIR_reg_t var; /* Used only internally */
    int64_t i;
    uint64_t u;
    float f;
    double d;
    long double ld;
    MIR_item_t ref; /* non-export/non-forward after simplification */
    MIR_str_t str;
    MIR_mem_t mem;
    MIR_mem_t var_mem; /* Used only internally */
    MIR_label_t label;
  } u;
} MIR_op_t;

typedef struct MIR_insn *MIR_insn_t;

/* Definition of link of double list of insns */
DEF_DLIST_LINK (MIR_insn_t);

struct MIR_insn {
  void *data; /* Aux data */
  DLIST_LINK (MIR_insn_t) insn_link;
  MIR_insn_code_t code : 32;
  unsigned int nops : 32; /* number of operands */
  MIR_op_t ops[1];
};

/* Definition of double list of insns */
DEF_DLIST (MIR_insn_t, insn_link);

typedef struct MIR_var {
  MIR_type_t type; /* MIR_T_BLK .. MIR_T_RBLK can be used only args */
  const char *name;
  size_t size; /* ignored for type != [MIR_T_BLK .. MIR_T_RBLK] */
} MIR_var_t;

DEF_VARR (MIR_var_t);

/* Function definition */
typedef struct MIR_func {
  const char *name;
  MIR_item_t func_item;
  size_t original_vars_num;
  DLIST (MIR_insn_t) insns, original_insns;
  uint32_t nres, nargs, last_temp_num, n_inlines;
  MIR_type_t *res_types;
  char vararg_p;                  /* flag of variable number of arguments */
  char expr_p;                    /* flag of that the func can be used as a linker expression */
  char jret_p;                    /* flag of jcall/jret func, set up after MIR_func_finish */
  VARR (MIR_var_t) * vars;        /* args and locals but temps */
  VARR (MIR_var_t) * global_vars; /* can be NULL */
  void *machine_code;             /* address of generated machine code or NULL */
  void *call_addr; /* address to call the function, it can be the same as machine_code */
  void *internal;  /* internal data structure */
  struct MIR_lref_data *first_lref; /* label addr data of the func: defined by module load */
} *MIR_func_t;

typedef struct MIR_proto {
  const char *name;
  uint32_t nres;
  MIR_type_t *res_types;   /* != MIR_T_UNDEF */
  char vararg_p;           /* flag of variable number of arguments */
  VARR (MIR_var_t) * args; /* args name can be NULL */
} *MIR_proto_t;

typedef struct MIR_data {
  const char *name; /* can be NULL */
  MIR_type_t el_type;
  size_t nel;
  union {
    long double d; /* for alignment of temporary literals */
    uint8_t els[1];
  } u;
} *MIR_data_t;

typedef struct MIR_ref_data {
  const char *name;    /* can be NULL */
  MIR_item_t ref_item; /* base */
  int64_t disp;        /* disp relative to base */
  void *load_addr;
} *MIR_ref_data_t;

typedef struct MIR_lref_data { /* describing [name:]lref lab[,label2][,disp] = lab-lab2+disp */
  const char *name;            /* can be NULL */
  MIR_label_t label;           /* base */
  MIR_label_t label2;          /* can be NULL */
  MIR_label_t orig_label, orig_label2; /* used to restore original func lrefs */
  int64_t disp;                        /* disp relative to base */
  void *load_addr;                     /* where is the value placed */
  struct MIR_lref_data *next;          /* next label addr related to the same func */
} *MIR_lref_data_t;

typedef struct MIR_expr_data {
  const char *name;     /* can be NULL */
  MIR_item_t expr_item; /* a special function can be called during linking */
  void *load_addr;
} *MIR_expr_data_t;

typedef struct MIR_bss {
  const char *name; /* can be NULL */
  uint64_t len;
} *MIR_bss_t;

typedef struct MIR_module *MIR_module_t;

/* Definition of link of double list of MIR_item_t type elements */
DEF_DLIST_LINK (MIR_item_t);

#define ITEM_EL(i) MIR_##i##_item

typedef enum {
  REP8 (ITEM_EL, func, proto, import, export, forward, data, ref_data, lref_data),
  REP2 (ITEM_EL, expr_data, bss),
} MIR_item_type_t;

#undef ERR_EL
#undef INSN_EL
#undef TYPE_EL
#undef OP_EL
#undef ITEM_EL
#undef REP_SEP

/* MIR module items (function or import): */
struct MIR_item {
  void *data;
  MIR_module_t module;
  DLIST_LINK (MIR_item_t) item_link;
  MIR_item_type_t item_type; /* item type */
  /* Non-null only for export/forward items and import item after
     linking.  It forms a chain to the final definition. */
  MIR_item_t ref_def;
  /* address of loaded data/bss items, function to call the function
     item, imported definition or proto object */
  void *addr;
  char export_p; /* true for export items (only func items) */
  /* defined for data-bss after loading. True if it is a start of allocated section */
  char section_head_p;
  union {
    MIR_func_t func;
    MIR_proto_t proto;
    MIR_name_t import_id;
    MIR_name_t export_id;
    MIR_name_t forward_id;
    MIR_data_t data;
    MIR_ref_data_t ref_data;
    MIR_lref_data_t lref_data;
    MIR_expr_data_t expr_data;
    MIR_bss_t bss;
  } u;
};

/* Definition of double list of MIR_item_t type elements */
DEF_DLIST (MIR_item_t, item_link);

/* Definition of link of double list of MIR_module_t type elements */
DEF_DLIST_LINK (MIR_module_t);

/* MIR module: */
struct MIR_module {
  void *data;
  const char *name;
  DLIST (MIR_item_t) items; /* module items */
  DLIST_LINK (MIR_module_t) module_link;
  uint32_t last_temp_item_num; /* Used only internally */
};

/* Definition of double list of MIR_item_t type elements */
DEF_DLIST (MIR_module_t, module_link);

struct MIR_context;
typedef struct MIR_context *MIR_context_t;

static inline int MIR_FP_branch_code_p (MIR_insn_code_t code) {
  return (code == MIR_FBEQ || code == MIR_DBEQ || code == MIR_LDBEQ || code == MIR_FBNE
          || code == MIR_DBNE || code == MIR_LDBNE || code == MIR_FBLT || code == MIR_DBLT
          || code == MIR_LDBLT || code == MIR_FBLE || code == MIR_DBLE || code == MIR_LDBLE
          || code == MIR_FBGT || code == MIR_DBGT || code == MIR_LDBGT || code == MIR_FBGE
          || code == MIR_DBGE || code == MIR_LDBGE);
}

static inline int MIR_call_code_p (MIR_insn_code_t code) {
  return code == MIR_CALL || code == MIR_INLINE || code == MIR_JCALL;
}

static inline int MIR_int_branch_code_p (MIR_insn_code_t code) {
  return (code == MIR_BT || code == MIR_BTS || code == MIR_BF || code == MIR_BFS || code == MIR_BEQ
          || code == MIR_BEQS || code == MIR_BNE || code == MIR_BNES || code == MIR_BLT
          || code == MIR_BLTS || code == MIR_UBLT || code == MIR_UBLTS || code == MIR_BLE
          || code == MIR_BLES || code == MIR_UBLE || code == MIR_UBLES || code == MIR_BGT
          || code == MIR_BGTS || code == MIR_UBGT || code == MIR_UBGTS || code == MIR_BGE
          || code == MIR_BGES || code == MIR_UBGE || code == MIR_UBGES || code == MIR_BO
          || code == MIR_UBO || code == MIR_BNO || code == MIR_UBNO);
}

static inline int MIR_branch_code_p (MIR_insn_code_t code) {
  return (code == MIR_JMP || MIR_int_branch_code_p (code) || MIR_FP_branch_code_p (code));
}

static inline int MIR_any_branch_code_p (MIR_insn_code_t code) {
  return (MIR_branch_code_p (code) || code == MIR_JMPI || code == MIR_SWITCH);
}

static inline int MIR_addr_code_p (MIR_insn_code_t code) {
  return (code == MIR_ADDR || code == MIR_ADDR8 || code == MIR_ADDR16 || code == MIR_ADDR32);
}

static inline int MIR_overflow_insn_code_p (MIR_insn_code_t code) {
  return (code == MIR_ADDO || code == MIR_ADDOS || code == MIR_SUBO || code == MIR_SUBOS
          || code == MIR_MULO || code == MIR_MULOS || code == MIR_UMULO || code == MIR_UMULOS);
}

extern double _MIR_get_api_version (void);
extern MIR_context_t _MIR_init (MIR_alloc_t alloc, MIR_code_alloc_t code_alloc);

/* Use only either the following API to create MIR code... */
static inline MIR_context_t MIR_init2 (MIR_alloc_t alloc, MIR_code_alloc_t code_alloc) {
  if (MIR_API_VERSION != _MIR_get_api_version ()) {
    fprintf (stderr,
             "mir.h header has version %g different from used mir code version %g -- good bye!\n",
             MIR_API_VERSION, _MIR_get_api_version ());
    exit (1);
  }
  return _MIR_init (alloc, code_alloc);
}

/* ...or this one. */
static inline MIR_context_t MIR_init (void) {
  return MIR_init2 (NULL, NULL);
}

extern void MIR_finish (MIR_context_t ctx);

extern MIR_module_t MIR_new_module (MIR_context_t ctx, const char *name);
extern DLIST (MIR_module_t) * MIR_get_module_list (MIR_context_t ctx);
extern MIR_item_t MIR_new_import (MIR_context_t ctx, const char *name);
extern MIR_item_t MIR_new_export (MIR_context_t ctx, const char *name);
extern MIR_item_t MIR_new_forward (MIR_context_t ctx, const char *name);
extern MIR_item_t MIR_new_bss (MIR_context_t ctx, const char *name,
                               size_t len); /* name can be NULL */
extern MIR_item_t MIR_new_data (MIR_context_t ctx, const char *name, MIR_type_t el_type, size_t nel,
                                const void *els); /* name can be NULL */
extern MIR_item_t MIR_new_string_data (MIR_context_t ctx, const char *name,
                                       MIR_str_t str); /* name can be NULL */
extern MIR_item_t MIR_new_ref_data (MIR_context_t ctx, const char *name, MIR_item_t item,
                                    int64_t disp); /* name can be NULL */
extern MIR_item_t MIR_new_lref_data (MIR_context_t ctx, const char *name, MIR_label_t label,
                                     MIR_label_t label2,
                                     int64_t disp); /* name and label2 can be NULL */
extern MIR_item_t MIR_new_expr_data (MIR_context_t ctx, const char *name,
                                     MIR_item_t expr_item); /* name can be NULL */
extern MIR_item_t MIR_new_proto_arr (MIR_context_t ctx, const char *name, size_t nres,
                                     MIR_type_t *res_types, size_t nargs, MIR_var_t *vars);
extern MIR_item_t MIR_new_proto (MIR_context_t ctx, const char *name, size_t nres,
                                 MIR_type_t *res_types, size_t nargs, ...);
extern MIR_item_t MIR_new_vararg_proto_arr (MIR_context_t ctx, const char *name, size_t nres,
                                            MIR_type_t *res_types, size_t nargs, MIR_var_t *vars);
extern MIR_item_t MIR_new_vararg_proto (MIR_context_t ctx, const char *name, size_t nres,
                                        MIR_type_t *res_types, size_t nargs, ...);
extern MIR_item_t MIR_new_func_arr (MIR_context_t ctx, const char *name, size_t nres,
                                    MIR_type_t *res_types, size_t nargs, MIR_var_t *vars);
extern MIR_item_t MIR_new_func (MIR_context_t ctx, const char *name, size_t nres,
                                MIR_type_t *res_types, size_t nargs, ...);
extern MIR_item_t MIR_new_vararg_func_arr (MIR_context_t ctx, const char *name, size_t nres,
                                           MIR_type_t *res_types, size_t nargs, MIR_var_t *vars);
extern MIR_item_t MIR_new_vararg_func (MIR_context_t ctx, const char *name, size_t nres,
                                       MIR_type_t *res_types, size_t nargs, ...);
extern const char *MIR_item_name (MIR_context_t ctx, MIR_item_t item);
extern MIR_func_t MIR_get_item_func (MIR_context_t ctx, MIR_item_t item);
extern MIR_reg_t MIR_new_func_reg (MIR_context_t ctx, MIR_func_t func, MIR_type_t type,
                                   const char *name);
extern MIR_reg_t MIR_new_global_func_reg (MIR_context_t ctx, MIR_func_t func, MIR_type_t type,
                                          const char *name, const char *hard_reg_name);
extern void MIR_finish_func (MIR_context_t ctx);
extern void MIR_finish_module (MIR_context_t ctx);

extern MIR_error_func_t MIR_get_error_func (MIR_context_t ctx);
extern void MIR_set_error_func (MIR_context_t ctx, MIR_error_func_t func);

extern MIR_alloc_t MIR_get_alloc (MIR_context_t ctx);

extern int MIR_get_func_redef_permission_p (MIR_context_t ctx);
extern void MIR_set_func_redef_permission (MIR_context_t ctx, int flag_p);

extern MIR_insn_t MIR_new_insn_arr (MIR_context_t ctx, MIR_insn_code_t code, size_t nops,
                                    MIR_op_t *ops);
extern MIR_insn_t MIR_new_insn (MIR_context_t ctx, MIR_insn_code_t code, ...);
extern MIR_insn_t MIR_new_call_insn (MIR_context_t ctx, size_t nops, ...);
extern MIR_insn_t MIR_new_jcall_insn (MIR_context_t ctx, size_t nops, ...);
extern MIR_insn_t MIR_new_ret_insn (MIR_context_t ctx, size_t nops, ...);
extern MIR_insn_t MIR_copy_insn (MIR_context_t ctx, MIR_insn_t insn);

extern const char *MIR_insn_name (MIR_context_t ctx, MIR_insn_code_t code);
extern size_t MIR_insn_nops (MIR_context_t ctx, MIR_insn_t insn);
extern MIR_op_mode_t MIR_insn_op_mode (MIR_context_t ctx, MIR_insn_t insn, size_t nop, int *out_p);

extern MIR_insn_t MIR_new_label (MIR_context_t ctx);

extern MIR_reg_t MIR_reg (MIR_context_t ctx, const char *reg_name, MIR_func_t func);
extern MIR_type_t MIR_reg_type (MIR_context_t ctx, MIR_reg_t reg, MIR_func_t func);
extern const char *MIR_reg_name (MIR_context_t ctx, MIR_reg_t reg, MIR_func_t func);
extern const char *MIR_reg_hard_reg_name (MIR_context_t ctx, MIR_reg_t reg, MIR_func_t func);

extern const char *MIR_alias_name (MIR_context_t ctx, MIR_alias_t alias);
extern MIR_alias_t MIR_alias (MIR_context_t ctx, const char *name);

extern MIR_op_t MIR_new_reg_op (MIR_context_t ctx, MIR_reg_t reg);
extern MIR_op_t MIR_new_int_op (MIR_context_t ctx, int64_t v);
extern MIR_op_t MIR_new_uint_op (MIR_context_t ctx, uint64_t v);
extern MIR_op_t MIR_new_float_op (MIR_context_t ctx, float v);
extern MIR_op_t MIR_new_double_op (MIR_context_t ctx, double v);
extern MIR_op_t MIR_new_ldouble_op (MIR_context_t ctx, long double v);
extern MIR_op_t MIR_new_ref_op (MIR_context_t ctx, MIR_item_t item);
extern MIR_op_t MIR_new_str_op (MIR_context_t ctx, MIR_str_t str);
extern MIR_op_t MIR_new_mem_op (MIR_context_t ctx, MIR_type_t type, MIR_disp_t disp, MIR_reg_t base,
                                MIR_reg_t index, MIR_scale_t scale);
extern MIR_op_t MIR_new_alias_mem_op (MIR_context_t ctx, MIR_type_t type, MIR_disp_t disp,
                                      MIR_reg_t base, MIR_reg_t index, MIR_scale_t scale,
                                      MIR_alias_t alias, MIR_alias_t noalias);
extern MIR_op_t MIR_new_label_op (MIR_context_t ctx, MIR_label_t label);
extern int MIR_op_eq_p (MIR_context_t ctx, MIR_op_t op1, MIR_op_t op2);
extern htab_hash_t MIR_op_hash_step (MIR_context_t ctx, htab_hash_t h, MIR_op_t op);

extern void MIR_append_insn (MIR_context_t ctx, MIR_item_t func, MIR_insn_t insn);
extern void MIR_prepend_insn (MIR_context_t ctx, MIR_item_t func, MIR_insn_t insn);
extern void MIR_insert_insn_after (MIR_context_t ctx, MIR_item_t func, MIR_insn_t after,
                                   MIR_insn_t insn);
extern void MIR_insert_insn_before (MIR_context_t ctx, MIR_item_t func, MIR_insn_t before,
                                    MIR_insn_t insn);
extern void MIR_remove_insn (MIR_context_t ctx, MIR_item_t func, MIR_insn_t insn);

extern void MIR_change_module_ctx (MIR_context_t old_ctx, MIR_module_t m, MIR_context_t new_ctx);

extern MIR_insn_code_t MIR_reverse_branch_code (MIR_insn_code_t code);

extern const char *MIR_type_str (MIR_context_t ctx, MIR_type_t tp);
extern void MIR_output_str (MIR_context_t ctx, FILE *f, MIR_str_t str);
extern void MIR_output_op (MIR_context_t ctx, FILE *f, MIR_op_t op, MIR_func_t func);
extern void MIR_output_insn (MIR_context_t ctx, FILE *f, MIR_insn_t insn, MIR_func_t func,
                             int newline_p);
extern void MIR_output_item (MIR_context_t ctx, FILE *f, MIR_item_t item);
extern void MIR_output_module (MIR_context_t ctx, FILE *f, MIR_module_t module);
extern void MIR_output (MIR_context_t ctx, FILE *f);

#if !MIR_NO_IO
extern void MIR_write (MIR_context_t ctx, FILE *f);
extern void MIR_write_module (MIR_context_t ctx, FILE *f, MIR_module_t module);
extern void MIR_read (MIR_context_t ctx, FILE *f);
extern void MIR_write_with_func (MIR_context_t ctx,
                                 int (*const writer_func) (MIR_context_t, uint8_t));
extern void MIR_write_module_with_func (MIR_context_t ctx,
                                        int (*const writer_func) (MIR_context_t, uint8_t),
                                        MIR_module_t module);
extern void MIR_read_with_func (MIR_context_t ctx, int (*const reader_func) (MIR_context_t));
#endif

#if !MIR_NO_SCAN
extern void MIR_scan_string (MIR_context_t ctx, const char *str);
#endif

extern MIR_item_t MIR_get_global_item (MIR_context_t ctx, const char *name);
extern void MIR_load_module (MIR_context_t ctx, MIR_module_t m);
extern void MIR_load_external (MIR_context_t ctx, const char *name, void *addr);
extern void MIR_link (MIR_context_t ctx, void (*set_interface) (MIR_context_t ctx, MIR_item_t item),
                      void *(*import_resolver) (const char *) );

/* Interpreter: */
typedef union {
  MIR_insn_code_t ic;
  void *a;
  int64_t i;
  uint64_t u;
  float f;
  double d;
  long double ld;
} MIR_val_t;

extern void MIR_interp (MIR_context_t ctx, MIR_item_t func_item, MIR_val_t *results, size_t nargs,
                        ...);
extern void MIR_interp_arr (MIR_context_t ctx, MIR_item_t func_item, MIR_val_t *results,
                            size_t nargs, MIR_val_t *vals);
extern void MIR_interp_arr_varg (MIR_context_t ctx, MIR_item_t func_item, MIR_val_t *results,
                                 size_t nargs, MIR_val_t *vals, va_list va);
extern void MIR_set_interp_interface (MIR_context_t ctx, MIR_item_t func_item);

/* Private: */
extern double _MIR_get_api_version (void);
extern MIR_context_t _MIR_init (MIR_alloc_t alloc, MIR_code_alloc_t code_alloc);
extern const char *_MIR_uniq_string (MIR_context_t ctx, const char *str);
extern int _MIR_reserved_ref_name_p (MIR_context_t ctx, const char *name);
extern int _MIR_reserved_name_p (MIR_context_t ctx, const char *name);
extern int64_t _MIR_addr_offset (MIR_context_t ctx, MIR_insn_code_t code);
extern void _MIR_free_insn (MIR_context_t ctx, MIR_insn_t insn);
extern MIR_reg_t _MIR_new_temp_reg (MIR_context_t ctx, MIR_type_t type,
                                    MIR_func_t func); /* for internal use only */
extern size_t _MIR_type_size (MIR_context_t ctx, MIR_type_t type);
extern MIR_op_mode_t _MIR_insn_code_op_mode (MIR_context_t ctx, MIR_insn_code_t code, size_t nop,
                                             int *out_p);
extern MIR_insn_t _MIR_new_unspec_insn (MIR_context_t ctx, size_t nops, ...);
extern void _MIR_register_unspec_insn (MIR_context_t ctx, uint64_t code, const char *name,
                                       size_t nres, MIR_type_t *res_types, size_t nargs,
                                       int vararg_p, MIR_var_t *args);
extern void _MIR_duplicate_func_insns (MIR_context_t ctx, MIR_item_t func_item);
extern void _MIR_restore_func_insns (MIR_context_t ctx, MIR_item_t func_item);

extern void _MIR_output_data_item_els (MIR_context_t ctx, FILE *f, MIR_item_t item, int c_p);
extern void _MIR_get_temp_item_name (MIR_context_t ctx, MIR_module_t module, char *buff,
                                     size_t buff_len);

extern MIR_op_t _MIR_new_var_op (MIR_context_t ctx, MIR_reg_t var);

extern MIR_op_t _MIR_new_var_mem_op (MIR_context_t ctx, MIR_type_t type, MIR_disp_t disp,
                                     MIR_reg_t base, MIR_reg_t index, MIR_scale_t scale);
extern MIR_op_t _MIR_new_alias_var_mem_op (MIR_context_t ctx, MIR_type_t type, MIR_disp_t disp,
                                           MIR_reg_t base, MIR_reg_t index, MIR_scale_t scale,
                                           MIR_alias_t alias, MIR_alias_t no_alias);

extern MIR_item_t _MIR_builtin_proto (MIR_context_t ctx, MIR_module_t module, const char *name,
                                      size_t nres, MIR_type_t *res_types, size_t nargs, ...);
extern MIR_item_t _MIR_builtin_func (MIR_context_t ctx, MIR_module_t module, const char *name,
                                     void *addr);
extern void _MIR_flush_code_cache (void *start, void *bound);
extern uint8_t *_MIR_publish_code (MIR_context_t ctx, const uint8_t *code, size_t code_len);
extern uint8_t *_MIR_get_new_code_addr (MIR_context_t ctx, size_t size);
extern uint8_t *_MIR_publish_code_by_addr (MIR_context_t ctx, void *addr, const uint8_t *code,
                                           size_t code_len);
struct MIR_code_reloc {
  size_t offset;
  const void *value;
};

typedef struct MIR_code_reloc MIR_code_reloc_t;

extern void _MIR_set_code (MIR_code_alloc_t alloc, size_t prot_start, size_t prot_len,
                           uint8_t *base, size_t nloc, const MIR_code_reloc_t *relocs,
                           size_t reloc_size);
extern void _MIR_change_code (MIR_context_t ctx, uint8_t *addr, const uint8_t *code,
                              size_t code_len);
extern void _MIR_update_code_arr (MIR_context_t ctx, uint8_t *base, size_t nloc,
                                  const MIR_code_reloc_t *relocs);
extern void _MIR_update_code (MIR_context_t ctx, uint8_t *base, size_t nloc, ...);

extern void *va_arg_builtin (void *p, uint64_t t);
extern void va_block_arg_builtin (void *res, void *p, size_t s, uint64_t t);
extern void va_start_interp_builtin (MIR_context_t ctx, void *p, void *a);
extern void va_end_interp_builtin (MIR_context_t ctx, void *p);

extern void *_MIR_get_bstart_builtin (MIR_context_t ctx);
extern void *_MIR_get_bend_builtin (MIR_context_t ctx);

typedef struct {
  MIR_type_t type;
  size_t size; /* used only for block arg (type == [MIR_T_BLK ..  MIR_T_RBLK]) */
} _MIR_arg_desc_t;

extern void *_MIR_get_ff_call (MIR_context_t ctx, size_t nres, MIR_type_t *res_types, size_t nargs,
                               _MIR_arg_desc_t *arg_descs, size_t arg_vars_num);
extern void *_MIR_get_interp_shim (MIR_context_t ctx, MIR_item_t func_item, void *handler);
extern void *_MIR_get_thunk (MIR_context_t ctx);
extern void *_MIR_get_thunk_addr (MIR_context_t ctx, void *thunk);
extern void _MIR_redirect_thunk (MIR_context_t ctx, void *thunk, void *to);
extern void *_MIR_get_jmpi_thunk (MIR_context_t ctx, void **res_loc, void *res, void *cont);
extern void *_MIR_get_wrapper (MIR_context_t ctx, MIR_item_t called_func, void *hook_address);
extern void *_MIR_get_wrapper_end (MIR_context_t ctx);
extern void *_MIR_get_bb_thunk (MIR_context_t ctx, void *bb_version, void *handler);
extern void _MIR_replace_bb_thunk (MIR_context_t ctx, void *thunk, void *to);
extern void *_MIR_get_bb_wrapper (MIR_context_t ctx, void *data, void *hook_address);

extern int _MIR_name_char_p (MIR_context_t ctx, int ch, int first_p);
extern void _MIR_dump_code (const char *name, uint8_t *code, size_t code_len);

extern int _MIR_get_hard_reg (MIR_context_t ctx, const char *hard_reg_name);
extern void *_MIR_get_module_global_var_hard_regs (MIR_context_t ctx, MIR_module_t module);

#ifdef __cplusplus
}
#endif

#endif /* #ifndef MIR_H */
