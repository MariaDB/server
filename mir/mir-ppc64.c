/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include "mir-ppc64.h"
#include "mir-alloc.h"
#include "mir.h"

/* All BLK type values is passed in int regs, and if the regs are not enough, the rest is passed on
   the stack. RBLK is always passed by address.  */

#define VA_LIST_IS_ARRAY_P 1 /* one element which is a pointer to args */

static void ppc64_push_func_desc (MIR_alloc_t alloc, VARR (uint8_t) * *insn_varr);
void (*ppc64_func_desc) (MIR_alloc_t alloc, VARR (uint8_t) * *insn_varr) = ppc64_push_func_desc;

static void ppc64_push_func_desc (MIR_alloc_t alloc, VARR (uint8_t) * *insn_varr) {
  VARR_CREATE (uint8_t, *insn_varr, alloc, 128);
  for (int i = 0; i < PPC64_FUNC_DESC_LEN; i++)
    VARR_PUSH (uint8_t, *insn_varr, ((uint8_t *) ppc64_func_desc)[i]);
}

static void *ppc64_publish_func_and_redirect (MIR_context_t ctx, VARR (uint8_t) * insn_varr) {
  void *res
    = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, insn_varr), VARR_LENGTH (uint8_t, insn_varr));
  VARR_DESTROY (uint8_t, insn_varr);
  return res;
}

static void push_insns (VARR (uint8_t) * insn_varr, const uint32_t *pat, size_t pat_len) {
  uint8_t *p = (uint8_t *) pat;
  for (size_t i = 0; i < pat_len; i++) VARR_PUSH (uint8_t, insn_varr, p[i]);
}

static void ppc64_gen_mov (VARR (uint8_t) * insn_varr, unsigned to, unsigned from) {
  /* or to,from,from: */
  push_insn (insn_varr, (31 << 26) | (444 << 1) | (from << 21) | (to << 16) | (from << 11));
}

static void ppc64_gen_addi (VARR (uint8_t) * insn_varr, unsigned rt_reg, unsigned ra_reg,
                            int disp) {
  push_insn (insn_varr, (14 << 26) | (rt_reg << 21) | (ra_reg << 16) | (disp & 0xffff));
}

static void ppc64_gen_add (VARR (uint8_t) * insn_varr, unsigned rt_reg, unsigned ra_reg,
                           unsigned rb_reg) {
  push_insn (insn_varr, (31 << 26) | (266 << 1) | (rt_reg << 21) | (ra_reg << 16) | (rb_reg << 11));
}

static void ppc64_gen_ld (VARR (uint8_t) * insn_varr, unsigned to, unsigned base, int disp,
                          MIR_type_t type) {
  int single_p = type == MIR_T_F;
  int double_p = type == MIR_T_D || type == MIR_T_LD;
  /* (ld | lf[sd]) to, disp(base): */
  assert (base != 0 && base < 32 && to < 32 && (single_p || double_p || (disp & 0x3) == 0));
  push_insn (insn_varr, ((single_p   ? 48
                          : double_p ? 50
                                     : 58)
                         << 26)
                          | (to << 21) | (base << 16) | (disp & 0xffff));
}

static void ppc64_gen_st (VARR (uint8_t) * insn_varr, unsigned from, unsigned base, int disp,
                          MIR_type_t type) {
  int single_p = type == MIR_T_F;
  int double_p = type == MIR_T_D || type == MIR_T_LD;
  /* std|stf[sd] from, disp(base): */
  assert (base != 0 && base < 32 && from < 32 && (single_p || double_p || (disp & 0x3) == 0));
  push_insn (insn_varr, ((single_p   ? 52
                          : double_p ? 54
                                     : 62)
                         << 26)
                          | (from << 21) | (base << 16) | (disp & 0xffff));
}

static void ppc64_gen_stdu (VARR (uint8_t) * insn_varr, int disp) {
  assert ((disp & 0x3) == 0);
  push_insn (insn_varr, 0xf8210001 | (disp & 0xfffc)); /* stdu 1, disp (1) */
}

static void ppc64_gen_jump (VARR (uint8_t) * insn_varr, unsigned int reg) {
  push_insn (insn_varr, (31 << 26) | (467 << 1) | (reg << 21) | (9 << 16)); /* mctr reg */
  push_insn (insn_varr, (19 << 26) | (528 << 1) | (20 << 21));              /* bcctr */
}

static void ppc64_gen_call (VARR (uint8_t) * insn_varr, unsigned int reg) {
  if (reg != 12) ppc64_gen_mov (insn_varr, 12, reg);                       /* 12 = func addr */
  push_insn (insn_varr, (31 << 26) | (467 << 1) | (12 << 21) | (9 << 16)); /* mctr 12 */
  push_insn (insn_varr, (19 << 26) | (528 << 1) | (20 << 21) | 1);         /* bcctrl */
}

/* r11=addr_reg+addr_disp; r15=r1(sp)+sp_offset; r0=qwords-1;
   ctr=r0; L: r0=mem[r11]; r11+=8; mem[r15]=r0; r15+=8; bdnz L; */
static void gen_blk_mov (VARR (uint8_t) * insn_varr, size_t sp_offset, unsigned int addr_reg,
                         int addr_disp, size_t qwords) {
  static const uint32_t blk_mov_loop[] = {
    /*0:*/ 0x7c0903a6,  /*mctr r0*/
    /*4:*/ 0xe80b0000,  /*ld r0,0(r11)*/
    /*8:*/ 0x396b0008,  /*addi r11,r11,8*/
    /*12:*/ 0xf80f0000, /*std r0,0(r15)*/
    /*16:*/ 0x39ef0008, /*addi r15,r15,8*/
    /*20:*/ 0x4200fff0, /*bdnz 4*/
  };
  /* r11=addr_reg+addr_disp: */
  if (addr_reg != 11 || addr_disp != 0) ppc64_gen_addi (insn_varr, 11, addr_reg, addr_disp);
  if (sp_offset < 0x10000) {
    ppc64_gen_addi (insn_varr, 15, 1, sp_offset);
  } else {
    ppc64_gen_address (insn_varr, 15, (void *) sp_offset);
    ppc64_gen_add (insn_varr, 15, 15, 1);
  }
  ppc64_gen_address (insn_varr, 0, (void *) qwords); /*r0 = qwords*/
  push_insns (insn_varr, blk_mov_loop, sizeof (blk_mov_loop));
}

void *_MIR_get_bstart_builtin (MIR_context_t ctx) {
  static const uint32_t bstart_code[] = {
    0x7c230b78, /* mr 3,1 */
    0x4e800020, /* blr */
  };
  VARR (uint8_t) * code;

  ppc64_push_func_desc (ctx->alloc, &code);
  push_insns (code, bstart_code, sizeof (bstart_code));
  return ppc64_publish_func_and_redirect (ctx, code);
}

void *_MIR_get_bend_builtin (MIR_context_t ctx) {
  static const uint32_t bend_finish_code[] = {
    0x7c611b78, /* mr      r1,r3 */
    0x4e800020, /* blr */
  };
  VARR (uint8_t) * code;

  ppc64_push_func_desc (ctx->alloc, &code);
  ppc64_gen_ld (code, 0, 1, 0, MIR_T_I64);                /* r0 = 0(r1) */
  ppc64_gen_st (code, 0, 3, 0, MIR_T_I64);                /* 0(r3) = r0 */
  ppc64_gen_ld (code, 0, 1, PPC64_TOC_OFFSET, MIR_T_I64); /* r0 = toc_offset(r1) */
  ppc64_gen_st (code, 0, 3, PPC64_TOC_OFFSET, MIR_T_I64); /* toc_offset(r3) = r0 */
  push_insns (code, bend_finish_code, sizeof (bend_finish_code));
  return ppc64_publish_func_and_redirect (ctx, code);
}

static const int max_thunk_len = (7 * 4 + 8); /* 5 for r=addr, 2 for goto r, addr itself */

void *_MIR_get_thunk (MIR_context_t ctx) { /* emit 3 doublewords for func descriptor: */
  VARR (uint8_t) * code;
  void *res;

  VARR_CREATE (uint8_t, code, ctx->alloc, 128);
  for (int i = 0; i < max_thunk_len / 4; i++) push_insn (code, TARGET_NOP);
  res = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
  VARR_DESTROY (uint8_t, code);
  return res;
}

static const uint32_t thunk_code_end[] = {
  0x7d8903a6, /* mtctr r12 */
  0x4e800420, /* bctr */
};

void _MIR_redirect_thunk (MIR_context_t ctx, void *thunk, void *to) {
  VARR (uint8_t) * code;
  VARR_CREATE (uint8_t, code, ctx->alloc, 256);
  ppc64_gen_address (code, 12, to);
  push_insns (code, thunk_code_end, sizeof (thunk_code_end));
  mir_assert ((VARR_LENGTH (uint8_t, code) & 0x3) == 0
              && VARR_LENGTH (uint8_t, code) <= (size_t) max_thunk_len);
  push_insns (code, (uint32_t *) &to, sizeof (to));
  _MIR_change_code (ctx, thunk, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
  VARR_DESTROY (uint8_t, code);
}

static void *get_jump_addr (uint32_t *insns) {
  int i;
  for (i = 0; i < 8; i++)
    if (insns[i] == 0x4e800420) break; /* bctr */
  mir_assert (i < 8);
  return (void *) (insns[i + 1] | ((uint64_t) insns[i + 2] << 32));
}

void *_MIR_get_thunk_addr (MIR_context_t ctx MIR_UNUSED, void *thunk) {
  return get_jump_addr (thunk);
}

struct ppc64_va_list {
  uint64_t *arg_area;
};

void *va_arg_builtin (void *p, uint64_t t) {
  struct ppc64_va_list *va = p;
  MIR_type_t type = t;
  void *a = va->arg_area;

  if (type == MIR_T_LD) {
    va->arg_area += 2;
  } else {
    va->arg_area++;
  }
  return a;
}

void va_block_arg_builtin (void *res, void *p, size_t s, uint64_t ncase MIR_UNUSED) {
  struct ppc64_va_list *va = p;
  void *a = va->arg_area;
  if (res != NULL) memcpy (res, a, s);
  va->arg_area += (s + sizeof (uint64_t) - 1) / sizeof (uint64_t);
}

void va_start_interp_builtin (MIR_context_t ctx MIR_UNUSED, void *p, void *a) {
  struct ppc64_va_list **va = p;
  va_list *vap = a;

  assert (sizeof (struct ppc64_va_list) == sizeof (va_list));
  *va = (struct ppc64_va_list *) vap;
}

void va_end_interp_builtin (MIR_context_t ctx MIR_UNUSED, void *p MIR_UNUSED) {}

/* Generation: fun (fun_addr, res_arg_addresses):
   save lr (r1 + 16); allocate and form minimal stack frame (with necessary param area); save
   r14,r15; r12=fun_addr (r3); r14 = res_arg_addresses (r4); r0=mem[r14,<args_offset>];
   (arg_reg=mem[r0] or r0=mem[r0];mem[r1,r1_offset]=r0) ... if func is vararg: put fp args also in
   gp regs call *r12; r0=mem[r14,<offset>]; res_reg=mem[r0]; ... restore r15, r14, r1, lr; return.
 */
void *_MIR_get_ff_call (MIR_context_t ctx, size_t nres, MIR_type_t *res_types, size_t nargs,
                        _MIR_arg_desc_t *arg_descs, size_t arg_vars_num) {
  static uint32_t start_pattern[] = {
    0x7c0802a6, /* mflr r0 */
    0xf8010010, /* std  r0,16(r1) */
  };
  static uint32_t finish_pattern[] = {
    0xe8010010, /* ld   r0,16(r1) */
    0x7c0803a6, /* mtlr r0 */
    0x4e800020, /* blr */
  };
  int vararg_p = nargs > arg_vars_num;
  MIR_type_t type;
  int n_gpregs = 0, n_fpregs = 0, res_reg = 14, qwords, frame_size;
  int disp, blk_disp, param_offset, param_size = 0;
  VARR (uint8_t) * code;

  ppc64_push_func_desc (ctx->alloc, &code);
  for (uint32_t i = 0; i < nargs; i++) {
    type = arg_descs[i].type;
    if (MIR_blk_type_p (type))
      param_size += (arg_descs[i].size + 7) / 8 * 8;
    else
      param_size += type == MIR_T_LD ? 16 : 8;
  }
  if (param_size < 64) param_size = 64;
  frame_size = PPC64_STACK_HEADER_SIZE + param_size + 16; /* +local var to save res_reg and 15 */
  if (frame_size % 16 != 0) frame_size += 8;              /* align */
  ppc64_gen_st (code, 2, 1, PPC64_TOC_OFFSET, MIR_T_I64);
  push_insns (code, start_pattern, sizeof (start_pattern));
  ppc64_gen_stdu (code, -frame_size);
  ppc64_gen_st (code, res_reg, 1, PPC64_STACK_HEADER_SIZE + param_size,
                MIR_T_I64); /* save res_reg */
  ppc64_gen_st (code, 15, 1, PPC64_STACK_HEADER_SIZE + param_size + 8, MIR_T_I64); /* save 15 */
  mir_assert (sizeof (long double) == 16);
  ppc64_gen_mov (code, res_reg, 4); /* results & args */
  ppc64_gen_mov (code, 12, 3);      /* func addr */
  n_gpregs = n_fpregs = 0;
  param_offset = nres * 16;              /* args start */
  disp = PPC64_STACK_HEADER_SIZE;        /* param area start */
  for (uint32_t i = 0; i < nargs; i++) { /* load args: */
    type = arg_descs[i].type;
    if ((type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD) && n_fpregs < 13) {
      ppc64_gen_ld (code, 1 + n_fpregs, res_reg, param_offset, type);
      if (vararg_p) {
        if (n_gpregs >= 8) {
          ppc64_gen_st (code, 1 + n_fpregs, 1, disp, MIR_T_D);
        } else { /* load into gp reg too */
          ppc64_gen_st (code, 1 + n_fpregs, 1, -8, MIR_T_D);
          ppc64_gen_ld (code, 3 + n_gpregs, 1, -8, MIR_T_I64);
        }
      }
      n_fpregs++;
      if (type == MIR_T_LD) {
        if (n_fpregs < 13) {
          ppc64_gen_ld (code, 1 + n_fpregs, res_reg, param_offset + 8, type);
          if (vararg_p) {
            if (n_gpregs + 1 >= 8) {
              ppc64_gen_st (code, 1 + n_fpregs, 1, disp + 8, MIR_T_D);
            } else { /* load gp reg to */
              ppc64_gen_st (code, 1 + n_fpregs, 1, -8, MIR_T_D);
              ppc64_gen_ld (code, 4 + n_gpregs, 1, -8, MIR_T_I64);
            }
          }
          n_fpregs++;
        } else {
          ppc64_gen_ld (code, 0, res_reg, param_offset + 8, type);
          ppc64_gen_st (code, 0, 1, disp + 8, MIR_T_D);
        }
      }
    } else if (type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD) {
      ppc64_gen_ld (code, 0, res_reg, param_offset, type);
      ppc64_gen_st (code, 0, 1, disp, MIR_T_D);
      if (type == MIR_T_LD) {
        ppc64_gen_ld (code, 0, res_reg, param_offset + 8, type);
        ppc64_gen_st (code, 0, 1, disp + 8, MIR_T_D);
      }
    } else if (MIR_blk_type_p (type)) {
      qwords = (arg_descs[i].size + 7) / 8;
      if (qwords > 0) ppc64_gen_ld (code, 11, res_reg, param_offset, MIR_T_I64);
      for (blk_disp = 0; qwords > 0 && n_gpregs < 8; qwords--, n_gpregs++, blk_disp += 8, disp += 8)
        ppc64_gen_ld (code, n_gpregs + 3, 11, blk_disp, MIR_T_I64);
      if (qwords > 0) gen_blk_mov (code, disp, 11, blk_disp, qwords);
      disp += qwords * 8;
      param_offset += 16;
      continue;
    } else if (n_gpregs < 8) { /* including RBLK */
      ppc64_gen_ld (code, n_gpregs + 3, res_reg, param_offset, MIR_T_I64);
    } else {
      ppc64_gen_ld (code, 0, res_reg, param_offset, MIR_T_I64);
      ppc64_gen_st (code, 0, 1, disp, MIR_T_I64);
    }
    disp += type == MIR_T_LD ? 16 : 8;
    param_offset += 16;
    n_gpregs += type == MIR_T_LD ? 2 : 1;
  }
  ppc64_gen_call (code, 12); /* call func_addr */
  n_gpregs = n_fpregs = 0;
  disp = 0;
  for (uint32_t i = 0; i < nres; i++) {
    type = res_types[i];
    if ((type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD) && n_fpregs < 8) {
      ppc64_gen_st (code, n_fpregs + 1, res_reg, disp, type);
      n_fpregs++;
      if (type == MIR_T_LD) {
        if (n_fpregs >= 8)
          MIR_get_error_func (ctx) (MIR_ret_error,
                                    "ppc64 can not handle this combination of return values");
        ppc64_gen_st (code, n_fpregs + 1, res_reg, disp + 8, type);
        n_fpregs++;
      }
    } else if (n_gpregs < 2) {  // just one-two gp reg
      ppc64_gen_st (code, n_gpregs + 3, res_reg, disp, MIR_T_I64);
      n_gpregs++;
    } else {
      MIR_get_error_func (ctx) (MIR_ret_error,
                                "ppc64 can not handle this combination of return values");
    }
    disp += 16;
  }
  ppc64_gen_ld (code, res_reg, 1, PPC64_STACK_HEADER_SIZE + param_size,
                MIR_T_I64); /* restore res_reg */
  ppc64_gen_ld (code, 15, 1, PPC64_STACK_HEADER_SIZE + param_size + 8, MIR_T_I64); /* restore r15 */
  ppc64_gen_addi (code, 1, 1, frame_size);
  push_insns (code, finish_pattern, sizeof (finish_pattern));
  return ppc64_publish_func_and_redirect (ctx, code);
}

/* Transform C call to call of void handler (MIR_context_t ctx, MIR_item_t func_item,
                                             va_list va, MIR_val_t *results):
   Brief: put all C call args to local vars (or if va_arg do nothing); save lr (r1+16), r14;
          allocate and form minimal shim stack frame (param area = 8 * 8);
          call handler with args; move results(r14) to return regs; restore lr,r14,r1; return */
void *_MIR_get_interp_shim (MIR_context_t ctx, MIR_item_t func_item, void *handler) {
  MIR_func_t func = func_item->u.func;
  uint32_t nres = func->nres, nargs = func->nargs;
  int vararg_p = func->vararg_p;
  MIR_type_t type, *res_types = func->res_types;
  MIR_var_t *arg_vars = VARR_ADDR (MIR_var_t, func->vars);
  int disp, start_disp, qwords, size, frame_size, local_var_size, param_offset;
  int va_reg = 11, caller_r1 = 12, res_reg = 14;
  int n_gpregs, n_fpregs;
  static uint32_t start_pattern[] = {
    0x7c0802a6, /* mflr r0 */
    0xf8010010, /* std  r0,16(r1) */
  };
  static uint32_t finish_pattern[] = {
    0xe8010010, /* ld   r0,16(r1) */
    0x7c0803a6, /* mtlr r0 */
    0x4e800020, /* blr */
  };
  VARR (uint8_t) * code;
  void *res;

  VARR_CREATE (uint8_t, code, ctx->alloc, 256);
  frame_size = PPC64_STACK_HEADER_SIZE + 64; /* header + 8(param area) */
  local_var_size = nres * 16 + 16;           /* saved r14, r15, results */
  if (vararg_p) {
    for (unsigned reg = 3; reg <= 10; reg++) /* std rn,dispn(r1) : */
      ppc64_gen_st (code, reg, 1, PPC64_STACK_HEADER_SIZE + (reg - 3) * 8, MIR_T_I64);
    ppc64_gen_addi (code, va_reg, 1, PPC64_STACK_HEADER_SIZE);
  } else {
    ppc64_gen_mov (code, caller_r1, 1); /* caller frame r1 */
    for (uint32_t i = 0; i < nargs; i++) {
      type = arg_vars[i].type;
      if (MIR_blk_type_p (type))
        local_var_size += (arg_vars[i].size + 7) / 8 * 8;
      else
        local_var_size += type == MIR_T_LD ? 16 : 8;
    }
  }
  frame_size += local_var_size;
  if (frame_size % 16 != 0) frame_size += 8; /* align */
  push_insns (code, start_pattern, sizeof (start_pattern));
  ppc64_gen_stdu (code, -frame_size);
  ppc64_gen_st (code, res_reg, 1, PPC64_STACK_HEADER_SIZE + 64, MIR_T_I64); /* save res_reg */
  ppc64_gen_st (code, 15, 1, PPC64_STACK_HEADER_SIZE + 72, MIR_T_I64);      /* save r15 */
  if (!vararg_p) { /* save args in local vars: */
    /* header_size + 64 + nres * 16 + 16 -- start of stack memory to keep args: */
    start_disp = disp = PPC64_STACK_HEADER_SIZE + 64 + nres * 16 + 16;
    param_offset = PPC64_STACK_HEADER_SIZE;
    n_gpregs = n_fpregs = 0;
    for (uint32_t i = 0; i < nargs; i++) {
      type = arg_vars[i].type;
      if ((type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD) && n_fpregs < 13) {
        ppc64_gen_st (code, n_fpregs + 1, 1, disp, MIR_T_D);
        n_fpregs++;
        if (type == MIR_T_LD) {
          if (n_fpregs < 13) {
            ppc64_gen_st (code, n_fpregs + 1, 1, disp + 8, MIR_T_D);
            n_fpregs++;
          } else {
            ppc64_gen_ld (code, 0, caller_r1, param_offset + 8, MIR_T_D);
            ppc64_gen_st (code, 0, 1, disp + 8, MIR_T_D);
          }
        }
      } else if (MIR_blk_type_p (type)) {
        qwords = (arg_vars[i].size + 7) / 8;
        for (; qwords > 0 && n_gpregs < 8; qwords--, n_gpregs++, disp += 8, param_offset += 8)
          ppc64_gen_st (code, n_gpregs + 3, 1, disp, MIR_T_I64);
        if (qwords > 0) {
          gen_blk_mov (code, disp, caller_r1, param_offset, qwords);
          disp += qwords * 8;
          param_offset += qwords * 8;
        }
        continue;
      } else if (n_gpregs < 8) {
        ppc64_gen_st (code, n_gpregs + 3, 1, disp, MIR_T_I64);
      } else if (type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD) {
        ppc64_gen_ld (code, 0, caller_r1, param_offset + (type == MIR_T_F ? 4 : 0), type);
        ppc64_gen_st (code, 0, 1, disp, MIR_T_D);
        if (type == MIR_T_LD) {
          ppc64_gen_ld (code, 0, caller_r1, param_offset + 8, MIR_T_D);
          ppc64_gen_st (code, 0, 1, disp + 8, MIR_T_D);
        }
      } else {
        ppc64_gen_ld (code, 0, caller_r1, param_offset, MIR_T_I64);
        ppc64_gen_st (code, 0, 1, disp, MIR_T_I64);
      }
      size = type == MIR_T_LD ? 16 : 8;
      disp += size;
      param_offset += size;
      n_gpregs += type == MIR_T_LD ? 2 : 1;
    }
    ppc64_gen_addi (code, va_reg, 1, start_disp);
  }
  ppc64_gen_addi (code, res_reg, 1, 64 + PPC64_STACK_HEADER_SIZE + 16);
  ppc64_gen_address (code, 3, ctx);
  ppc64_gen_address (code, 4, func_item);
  ppc64_gen_mov (code, 5, va_reg);
  ppc64_gen_mov (code, 6, res_reg);
  ppc64_gen_address (code, 12, handler);
  ppc64_gen_call (code, 12);
  disp = n_gpregs = n_fpregs = 0;
  for (uint32_t i = 0; i < nres; i++) {
    type = res_types[i];
    if ((type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD) && n_fpregs < 8) {
      ppc64_gen_ld (code, n_fpregs + 1, res_reg, disp, type);
      n_fpregs++;
      if (type == MIR_T_LD) {
        if (n_fpregs >= 8)
          MIR_get_error_func (ctx) (MIR_ret_error,
                                    "ppc64 can not handle this combination of return values");
        ppc64_gen_ld (code, n_fpregs + 1, res_reg, disp + 8, type);
        n_fpregs++;
      }
    } else if (n_gpregs < 2) {  // just one-two gp reg
      ppc64_gen_ld (code, n_gpregs + 3, res_reg, disp, MIR_T_I64);
      n_gpregs++;
    } else {
      MIR_get_error_func (ctx) (MIR_ret_error,
                                "ppc64 can not handle this combination of return values");
    }
    disp += 16;
  }
  ppc64_gen_ld (code, res_reg, 1, PPC64_STACK_HEADER_SIZE + 64, MIR_T_I64); /* restore res_reg */
  ppc64_gen_ld (code, 15, 1, PPC64_STACK_HEADER_SIZE + 72, MIR_T_I64);      /* restore r15 */
  ppc64_gen_addi (code, 1, 1, frame_size);
  push_insns (code, finish_pattern, sizeof (finish_pattern));
  res = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
  VARR_DESTROY (uint8_t, code);
  return res;
}

static void redirect_bb_thunk (MIR_context_t ctx, VARR (uint8_t) * code, void *start, void *to) {
  int64_t offset = (uint8_t *) to - (uint8_t *) start;
  mir_assert ((offset & 0x3) == 0);
  VARR_TRUNC (uint8_t, code, 0);
  if (((offset < 0 ? -offset : offset) & ~(int64_t) 0x1ffffff) == 0) { /* just jump */
    uint32_t insn
      = (PPC_JUMP_OPCODE << (32 - 6)) /* jump opcode */ | (((offset / 4) & 0xffffff) << 2);
    push_insn (code, insn);
  } else {
    ppc64_gen_address (code, 12, to); /* r12 = to */
    push_insns (code, thunk_code_end, sizeof (thunk_code_end));
    mir_assert ((VARR_LENGTH (uint8_t, code) & 0x3) == 0
                && VARR_LENGTH (uint8_t, code) <= (size_t) max_thunk_len);
  }
  _MIR_change_code (ctx, start, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
}

/* r11=<bb_version>; jump handler  ??? mutex free */
void *_MIR_get_bb_thunk (MIR_context_t ctx, void *bb_version, void *handler) {
  void *res;
  size_t offset;
  VARR (uint8_t) * code;

  VARR_CREATE (uint8_t, code, ctx->alloc, 64);
  ppc64_gen_address (code, 11, bb_version); /* x11 = bb_version */
  offset = VARR_LENGTH (uint8_t, code);
  for (int i = 0; i < max_thunk_len / 4; i++) push_insn (code, TARGET_NOP);
  res = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
  redirect_bb_thunk (ctx, code, (uint8_t *) res + offset, handler);
#if 0
  if (getenv ("MIR_code_dump") != NULL)
    _MIR_dump_code ("bb thunk:", res, offset + VARR_LENGTH (uint8_t, code));
#endif
  VARR_DESTROY (uint8_t, code);
  return res;
}

/* change to jump to */
void _MIR_replace_bb_thunk (MIR_context_t ctx, void *thunk, void *to) {
  size_t i, offset;
  VARR (uint8_t) * code;
  uint32_t opcode, *insns = (uint32_t *) thunk;

  /* find jump code offset (see ppc64_gen_address): */
  for (i = 0; i <= 5; i++) {
    if ((opcode = insns[i] >> 26) == (uint32_t) PPC_JUMP_OPCODE) break; /* uncond branch */
    if ((opcode == LI_OPCODE || opcode == LIS_OPCODE
         || opcode == XOR_OPCODE) /* (li|lis|xor) r12, ... */
        && ((insns[i] >> 21) & 0x1f) == 12)
      break;
  }
  mir_assert (i <= 5);
  offset = i * 4;
  VARR_CREATE (uint8_t, code, ctx->alloc, 64);
  redirect_bb_thunk (ctx, code, (char *) thunk + offset, to);
  VARR_DESTROY (uint8_t, code);
}

static const int wrapper_frame_size = PPC64_STACK_HEADER_SIZE + 8 * 8 + 13 * 8 + 8 * 8;

/* save lr(r1+16);update r1,save r3,r4 regs;r3=ctx;r4=called_func;r12=hook_address;jmp wrap_end */
void *_MIR_get_wrapper (MIR_context_t ctx, MIR_item_t called_func, void *hook_address) {
  static const uint32_t prologue[] = {
    0x7c0802a6, /* mflr r0 */
    0xf8010010, /* std  r0,16(r1) */
  };
  VARR (uint8_t) * code;
  void *res;
  int frame_size = wrapper_frame_size;

  VARR_CREATE (uint8_t, code, ctx->alloc, 256);
  push_insns (code, prologue, sizeof (prologue));
  /* stdu r1,n(r1): header + 8(gp args) + 13(fp args) + 8(param area): */
  if (frame_size % 16 != 0) frame_size += 8;
  ppc64_gen_stdu (code, -frame_size);
  for (unsigned reg = 3; reg <= 4; reg++) /* std rn,dispn(r1) : */
    ppc64_gen_st (code, reg, 1, PPC64_STACK_HEADER_SIZE + (reg - 3) * 8 + 64, MIR_T_I64);
  ppc64_gen_address (code, 3, ctx);
  ppc64_gen_address (code, 4, called_func);
  ppc64_gen_address (code, 12, hook_address);
  ppc64_gen_address (code, 11, wrapper_end_addr);
  ppc64_gen_jump (code, 11);
  res = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
#if 0
  if (getenv ("MIR_code_dump") != NULL)
    _MIR_dump_code ("wapper:", res, VARR_LENGTH (uint8_t, code));
#endif
  VARR_DESTROY (uint8_t, code);
  return res;
}

/* save all param regs but r3, r4; allocate and form minimal wrapper stack frame (param area = 8*8);
   r3 = call r12 (r3, r4); r12=r3; restore all params regs,r1,lr (r1+16);ctr=r12; b *ctr */
void *_MIR_get_wrapper_end (MIR_context_t ctx) {
  static const uint32_t epilogue[] = {
    0xe8010010, /* ld   r0,16(r1) */
    0x7c0803a6, /* mtlr r0 */
  };
  VARR (uint8_t) * code;
  void *res;
  int frame_size = wrapper_frame_size;

  if (frame_size % 16 != 0) frame_size += 8;
  VARR_CREATE (uint8_t, code, ctx->alloc, 256);
  for (unsigned reg = 5; reg <= 10; reg++) /* std rn,dispn(r1) : */
    ppc64_gen_st (code, reg, 1, PPC64_STACK_HEADER_SIZE + (reg - 3) * 8 + 64, MIR_T_I64);
  for (unsigned reg = 1; reg <= 13; reg++) /* stfd fn,dispn(r1) : */
    ppc64_gen_st (code, reg, 1, PPC64_STACK_HEADER_SIZE + (reg - 1 + 8) * 8 + 64, MIR_T_D);
  ppc64_gen_call (code, 12);
  ppc64_gen_mov (code, 12, 3);
  for (unsigned reg = 3; reg <= 10; reg++) /* ld rn,dispn(r1) : */
    ppc64_gen_ld (code, reg, 1, PPC64_STACK_HEADER_SIZE + (reg - 3) * 8 + 64, MIR_T_I64);
  for (unsigned reg = 1; reg <= 13; reg++) /* lfd fn,dispn(r1) : */
    ppc64_gen_ld (code, reg, 1, PPC64_STACK_HEADER_SIZE + (reg - 1 + 8) * 8 + 64, MIR_T_D);
  ppc64_gen_addi (code, 1, 1, frame_size);
  push_insns (code, epilogue, sizeof (epilogue));
  push_insn (code, (31 << 26) | (467 << 1) | (12 << 21) | (9 << 16)); /* mctr 12 */
  push_insn (code, (19 << 26) | (528 << 1) | (20 << 21));             /* bcctr */
  res = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
#if 0
  if (getenv ("MIR_code_dump") != NULL)
    _MIR_dump_code ("wapper end:", res, VARR_LENGTH (uint8_t, code));
#endif
  VARR_DESTROY (uint8_t, code);
  return res;
}

/* save all clobbered regs but r11 and r12; r11 = call hook_address (data, r11); restore regs; br
   r11 r11 is a generator temp reg which is not used across bb borders. */
void *_MIR_get_bb_wrapper (MIR_context_t ctx, void *data, void *hook_address) {
  static const uint32_t prologue[] = {
    0x7d800026, /* mfcr r12 */
    0xf9810008, /* std r12,8(r1) */
    0x7d8802a6, /* mflr r12 */
    0xf9810010, /* std  r12,16(r1) */
  };
  static const uint32_t epilogue[] = {
    0xe9810010, /* ld r12,16(r1) */
    0x7d8803a6, /* mtlr r12 */
    0xe9810008, /* ld r12,8(r1) */
    0x7d8ff120, /* mtcr r12 */
  };
  int frame_size = PPC64_STACK_HEADER_SIZE + 14 * 8 + 14 * 8 + 8 * 8;
  void *res;
  VARR (uint8_t) * code;

  VARR_CREATE (uint8_t, code, ctx->alloc, 256);
  push_insns (code, prologue, sizeof (prologue));
  /* stdu r1,n(r1): header + 14(gp regs, r{1,2,11} space alloc is not used) + 14(fp args) + 8(param
   * area): */
  if (frame_size % 16 != 0) frame_size += 8;
  ppc64_gen_stdu (code, -frame_size);
  ppc64_gen_st (code, R0_HARD_REG, SP_HARD_REG, PPC64_STACK_HEADER_SIZE + R0_HARD_REG * 8 + 64,
                MIR_T_I64);
  for (unsigned reg = R2_HARD_REG; reg <= R10_HARD_REG; reg++) /* ld rn,dispn(r1) : */
    ppc64_gen_st (code, reg, SP_HARD_REG, PPC64_STACK_HEADER_SIZE + reg * 8 + 64, MIR_T_I64);
  ppc64_gen_st (code, R13_HARD_REG, SP_HARD_REG, PPC64_STACK_HEADER_SIZE + R13_HARD_REG * 8 + 64,
                MIR_T_I64);
  for (unsigned reg = 0; reg <= F13_HARD_REG - F0_HARD_REG; reg++) /* lfd fn,dispn(r1) : */
    ppc64_gen_st (code, reg, SP_HARD_REG, PPC64_STACK_HEADER_SIZE + (reg + 14) * 8 + 64, MIR_T_D);
  ppc64_gen_address (code, 3, data);          /* r3 = data */
  ppc64_gen_mov (code, 4, 11);                /* r4 = r11 */
  ppc64_gen_address (code, 12, hook_address); /* r12 = hook addres */
  ppc64_gen_call (code, 12);                  /* call r12 */
  ppc64_gen_mov (code, 11, 3);                /* r11 = r3 */
  ppc64_gen_ld (code, R0_HARD_REG, SP_HARD_REG, PPC64_STACK_HEADER_SIZE + R0_HARD_REG * 8 + 64,
                MIR_T_I64);
  for (unsigned reg = R2_HARD_REG; reg <= R10_HARD_REG; reg++) /* ld rn,dispn(r1) : */
    ppc64_gen_ld (code, reg, SP_HARD_REG, PPC64_STACK_HEADER_SIZE + reg * 8 + 64, MIR_T_I64);
  ppc64_gen_ld (code, R13_HARD_REG, SP_HARD_REG, PPC64_STACK_HEADER_SIZE + R13_HARD_REG * 8 + 64,
                MIR_T_I64);
  for (unsigned reg = 0; reg <= F13_HARD_REG - F0_HARD_REG; reg++) /* lfd fn,dispn(r1) : */
    ppc64_gen_ld (code, reg, SP_HARD_REG, PPC64_STACK_HEADER_SIZE + (reg + 14) * 8 + 64, MIR_T_D);
  ppc64_gen_addi (code, 1, 1, frame_size);
  push_insns (code, epilogue, sizeof (epilogue));
  push_insn (code, (31 << 26) | (467 << 1) | (11 << 21) | (9 << 16)); /* mctr 11 */
  push_insn (code, (19 << 26) | (528 << 1) | (20 << 21));             /* bcctr */
  res = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
#if 0
  if (getenv ("MIR_code_dump") != NULL)
    _MIR_dump_code ("bb wrapper:", res, VARR_LENGTH (uint8_t, code));
#endif
  VARR_DESTROY (uint8_t, code);
  return res;
}
