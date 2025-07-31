/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include "mir-aarch64.h"

/* x31 - sp; x30 - link reg; x29 - fp; x0-x7, v0-v7 - arg/result regs;
   x19-x29, v8-v15 - callee-saved (only bottom 64-bits are saved for v8-v15);
   x9-x15, v0-v7, v16-v31 - temp regs
   x8 - indirect result location address
   stack is 16-byte aligned

   Apple M1 ABI specific:
   o long double is double (64-bit)
   o va_list is a pointer
   o all varargs are passed only on stack
   o reg x18 is reserved
   o empty struct args are ignored
*/

/* Any small BLK type (less or equal to two quadwords) args are passed in
   *fully* regs or on stack (w/o address), otherwise it is put
   somewhere on stack and its address passed instead. First RBLK arg
   is passed in r8. Other RBLK independently of size is always passed
   by address as an usual argument.  */

void *_MIR_get_bstart_builtin (MIR_context_t ctx) {
  static const uint32_t bstart_code[] = {
    0x910003e0, /* r0 = rsp */
    0xd65f03c0, /* ret r30 */
  };
  return _MIR_publish_code (ctx, (uint8_t *) bstart_code, sizeof (bstart_code));
}

void *_MIR_get_bend_builtin (MIR_context_t ctx) {
  static const uint32_t bend_code[] = {
    0x9100001f, /* rsp = r0 */
    0xd65f03c0, /* ret r30 */
  };
  return _MIR_publish_code (ctx, (uint8_t *) bend_code, sizeof (bend_code));
}

#define VA_LIST_IS_ARRAY_P 0
#if defined(__APPLE__)
struct aarch64_va_list {
  uint64_t *arg_area;
};
#else
struct aarch64_va_list {
  /* address following the last (highest addressed) named incoming
     argument on the stack, rounded upwards to a multiple of 8 bytes,
     or if there are no named arguments on the stack, then the value
     of the stack pointer when the function was entered. */
  void *__stack;
  /* the address of the byte immediately following the general
     register argument save area, the end of the save area being
     aligned to a 16 byte boundary. */
  void *__gr_top;
  /* the address of the byte immediately following the FP/SIMD
     register argument save area, the end of the save area being
     aligned to a 16 byte boundary. */
  void *__vr_top;
  int __gr_offs; /* set to 0 – ((8 – named_gr) * 8) */
  int __vr_offs; /* set to 0 – ((8 – named_vr) * 16) */
};
#endif

void *va_arg_builtin (void *p, uint64_t t) {
  struct aarch64_va_list *va = p;
  MIR_type_t type = t;
#if defined(__APPLE__)
  void *a = va->arg_area;

  if (type == MIR_T_LD && __SIZEOF_LONG_DOUBLE__ == 16) {
    va->arg_area += 2;
  } else {
    va->arg_area++;
  }
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  if (type == MIR_T_F || type == MIR_T_I32) a = (char *) a + 4; /* 2nd word of doubleword */
#endif
#else
  int fp_p = type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD;
  void *a;

  if (fp_p && va->__vr_offs < 0) {
    a = (char *) va->__vr_top + va->__vr_offs;
    va->__vr_offs += 16;
  } else if (!fp_p && va->__gr_offs < 0) {
    a = (char *) va->__gr_top + va->__gr_offs;
    va->__gr_offs += 8;
  } else {
    if (type == MIR_T_LD && __SIZEOF_LONG_DOUBLE__ == 16)
      va->__stack = (void *) (((uint64_t) va->__stack + 15) % 16);
    a = va->__stack;
    va->__stack
      = (char *) va->__stack + (type == MIR_T_LD && __SIZEOF_LONG_DOUBLE__ == 16 ? 16 : 8);
  }
#endif
  return a;
}

void va_block_arg_builtin (void *res, void *p, size_t s, uint64_t ncase MIR_UNUSED) {
  struct aarch64_va_list *va = p;
#if defined(__APPLE__)
  void *a = (void *) va->arg_area;
  if (s <= 2 * 8) {
    va->arg_area += (s + sizeof (uint64_t) - 1) / sizeof (uint64_t);
  } else {
    a = *(void **) a;
    va->arg_area++;
  }
  if (res != NULL) memcpy (res, a, s);
#else
  void *a;
  long size = (s + 7) / 8 * 8;

  if (size <= 2 * 8 && va->__gr_offs + size > 0) { /* not enough regs to pass: */
    a = va->__stack;
    va->__stack = (char *) va->__stack + size;
    va->__gr_offs += size;
    if (res != NULL) memcpy (res, a, s);
    return;
  }
  if (size > 2 * 8) size = 8;
  if (va->__gr_offs < 0) {
    a = (char *) va->__gr_top + va->__gr_offs;
    va->__gr_offs += size;
  } else {
    a = va->__stack;
    va->__stack = (char *) va->__stack + size;
  }
  if (s > 2 * 8) a = *(void **) a; /* address */
  if (res != NULL) memcpy (res, a, s);
#endif
}

void va_start_interp_builtin (MIR_context_t ctx MIR_UNUSED, void *p, void *a) {
  struct aarch64_va_list *va = p;
  va_list *vap = a;

  assert (sizeof (struct aarch64_va_list) == sizeof (va_list));
  *va = *(struct aarch64_va_list *) vap;
}

void va_end_interp_builtin (MIR_context_t ctx MIR_UNUSED, void *p MIR_UNUSED) {}

static int setup_imm64_insns (uint32_t *to, int reg, uint64_t imm64) {
  /* xd=imm64 */
  static const uint32_t imm64_pat[] = {
    0xd2800000, /*  0: mov xd, xxxx(0-15) */
    0xf2a00000, /*  4: movk xd, xxxx(16-31) */
    0xf2c00000, /*  8: movk xd, xxxx(32-47) */
    0xf2e00000, /* 12: movk xd, xxxx(48-63) */
  };
  uint32_t mask = ~(0xffff << 5);

  mir_assert (0 <= reg && reg <= 31);
  to[0] = (imm64_pat[0] & mask) | ((uint32_t) (imm64 & 0xffff) << 5) | reg;
  to[1] = (imm64_pat[1] & mask) | (((uint32_t) (imm64 >> 16) & 0xffff) << 5) | reg;
  to[2] = (imm64_pat[2] & mask) | (((uint32_t) (imm64 >> 32) & 0xffff) << 5) | reg;
  to[3] = (imm64_pat[3] & mask) | (((uint32_t) (imm64 >> 48) & 0xffff) << 5) | reg;
  return sizeof (imm64_pat) / sizeof (uint32_t);
}

static uint8_t *push_insns (VARR (uint8_t) * insn_varr, const uint32_t *pat, size_t pat_len) {
  uint8_t *p = (uint8_t *) pat;

  for (size_t i = 0; i < pat_len; i++) VARR_PUSH (uint8_t, insn_varr, p[i]);
  return VARR_ADDR (uint8_t, insn_varr) + VARR_LENGTH (uint8_t, insn_varr) - pat_len;
}

static size_t gen_mov_addr (VARR (uint8_t) * insn_varr, int reg, void *addr) {
  uint32_t insns[4];
  int insns_num = setup_imm64_insns (insns, reg, (uint64_t) addr);

  mir_assert (insns_num == 4 && sizeof (insns) == insns_num * sizeof (uint32_t));
  push_insns (insn_varr, insns, insns_num * sizeof (uint32_t));
  return insns_num * sizeof (uint32_t);
}

#define BR_OFFSET_BITS 26
#define MAX_BR_OFFSET (1 << (BR_OFFSET_BITS - 1)) /* 1 for sign */
#define BR_OFFSET_MASK (~(-1u << BR_OFFSET_BITS))

static void gen_call_addr (VARR (uint8_t) * insn_varr, void *base_addr, int temp_reg,
                           void *call_addr) {
  static const uint32_t call_pat1 = 0x94000000; /* bl x */
  static const uint32_t call_pat2 = 0xd63f0000; /* blr x */
  uint32_t insn;
  int64_t offset = (uint32_t *) call_addr - (uint32_t *) base_addr;

  mir_assert (0 <= temp_reg && temp_reg <= 31);
  if (base_addr != NULL && -(int64_t) MAX_BR_OFFSET <= offset && offset < (int64_t) MAX_BR_OFFSET) {
    insn = call_pat1 | ((uint32_t) offset & BR_OFFSET_MASK);
  } else {
    gen_mov_addr (insn_varr, temp_reg, call_addr);
    insn = call_pat2 | (temp_reg << 5);
  }
  push_insns (insn_varr, &insn, sizeof (insn));
}

void *_MIR_get_thunk (MIR_context_t ctx) {
  /* maximal size thunk -- see _MIR_redirect_thunk */
  int pat[4] = {TARGET_NOP, TARGET_NOP, TARGET_NOP, TARGET_NOP};

  return _MIR_publish_code (ctx, (uint8_t *) pat, sizeof (pat));
}

void _MIR_redirect_thunk (MIR_context_t ctx, void *thunk, void *to) {
  static const uint32_t branch_pat1 = 0xd61f0120; /* br x9 */
  static const uint32_t branch_pat2 = 0x14000000; /* b x */
  int64_t offset = (uint32_t *) to - (uint32_t *) thunk;
  uint32_t code[4];

  mir_assert (((uint64_t) thunk & 0x3) == 0 && ((uint64_t) to & 0x3) == 0); /* alignment */
  if (-(int64_t) MAX_BR_OFFSET <= offset && offset < (int64_t) MAX_BR_OFFSET) {
    code[0] = branch_pat2 | ((uint32_t) offset & BR_OFFSET_MASK);
    _MIR_change_code (ctx, thunk, (uint8_t *) &code[0], sizeof (code[0]));
  } else {
    code[0] = 0x58000049; /* ldr x9,8 (pc-relative) */
    code[1] = branch_pat1;
    *(void **) &code[2] = to;
    _MIR_change_code (ctx, thunk, (uint8_t *) code, sizeof (code));
  }
}

void *_MIR_get_thunk_addr (MIR_context_t ctx MIR_UNUSED, void *thunk) {
  void *addr;
  int short_p = (*(uint32_t *) thunk >> BR_OFFSET_BITS) == 0x5;
  if (short_p) {
    int32_t offset = *(uint32_t *) thunk & BR_OFFSET_MASK;
    addr = (uint8_t *) thunk + ((offset << (32 - BR_OFFSET_BITS)) >> (30 - BR_OFFSET_BITS));
  } else {
    addr = *(void **) ((char *) thunk + 8);
  }
  return addr;
}

static void gen_blk_mov (VARR (uint8_t) * insn_varr, uint32_t offset, uint32_t addr_offset,
                         uint32_t qwords, uint32_t addr_reg) {
  static const uint32_t blk_mov_pat[] = {
    /* 0:*/ 0xf940026c,  /* ldr x12, [x19,<addr_offset>]*/
    /* 4:*/ 0x910003e0,  /* add <addr_reg>, sp, <offset>*/
    /* 8:*/ 0xd280000b,  /* mov x11, 0*/
    /* c:*/ 0xd280000e,  /* mov x14, <qwords>*/
    /* 10:*/ 0xf86c696a, /* ldr x10, [x11,x12]*/
    /* 14:*/ 0xd10005ce, /* sub x14, x14, #0x1*/
    /* 18:*/ 0xf820696a, /* str x10, [x11,<addr_reg>x13]*/
    /* 1c:*/ 0xf10001df, /* cmp x14, 0*/
    /* 20:*/ 0x9100216b, /* add x11, x11, 8*/
    /* 24:*/ 0x54ffff61, /* b.ne 10 */
  };
  if (qwords == 0) {
    uint32_t pat = 0x910003e0 | addr_reg | (offset << 10); /* add <add_reg>, sp, <offset>*/
    push_insns (insn_varr, &pat, sizeof (pat));
  } else {
    uint32_t *addr = (uint32_t *) push_insns (insn_varr, blk_mov_pat, sizeof (blk_mov_pat));
    mir_assert (offset < (1 << 12) && addr_offset % 8 == 0 && (addr_offset >> 3) < (1 << 12));
    mir_assert (addr_reg < 32 && qwords < (1 << 16));
    addr[0] |= (addr_offset >> 3) << 10;
    addr[1] |= addr_reg | (offset << 10);
    addr[3] |= qwords << 5;
    addr[6] |= addr_reg << 16;
  }
}

static const uint32_t save_insns[] = {
  /* save r0-r8,v0-v7 */
  0xa9bf1fe6, /* stp R6, R7, [SP, #-16]! */
  0xa9bf17e4, /* stp R4, R5, [SP, #-16]! */
  0xa9bf0fe2, /* stp R2, R3, [SP, #-16]! */
  0xa9bf07e0, /* stp R0, R1, [SP, #-16]! */
  0xd10043ff, /* sub SP, SP, #16 */
  0xf90007e8, /* str x8, [SP, #8] */
  0xadbf1fe6, /* stp Q6, Q7, [SP, #-32]! */
  0xadbf17e4, /* stp Q4, Q5, [SP, #-32]! */
  0xadbf0fe2, /* stp Q2, Q3, [SP, #-32]! */
  0xadbf07e0, /* stp Q0, Q1, [SP, #-32]! */
};
static const uint32_t restore_insns[] = {
  /* restore r0-r8,v0-v7 */
  0xacc107e0, /* ldp Q0, Q1, SP, #32 */
  0xacc10fe2, /* ldp Q2, Q3, SP, #32 */
  0xacc117e4, /* ldp Q4, Q5, SP, #32 */
  0xacc11fe6, /* ldp Q6, Q7, SP, #32 */
  0xf94007e8, /* ldr x8, [SP, #8] */
  0x910043ff, /* add SP, SP, #16 */
  0xa8c107e0, /* ldp R0, R1, SP, #16 */
  0xa8c10fe2, /* ldp R2, R3, SP, #16 */
  0xa8c117e4, /* ldp R4, R5, SP, #16 */
  0xa8c11fe6, /* ldp R6, R7, SP, #16 */
};

static const uint32_t ld_pat = 0xf9400260;     /* ldr x, [x19], offset */
static const uint32_t lds_pat = 0xbd400260;    /* ldr s, [x19], offset */
static const uint32_t ldd_pat = 0xfd400260;    /* ldr d, [x19], offset */
static const uint32_t ldld_pat = 0x3dc00260;   /* ldr q, [x19], offset */
static const uint32_t gen_ld_pat = 0xf9400000; /* ldr x, [xn|sp], offset */

static const uint32_t st_pat = 0xf9000000;   /* str x, [xn|sp], offset */
static const uint32_t sts_pat = 0xbd000000;  /* str s, [xn|sp], offset */
static const uint32_t std_pat = 0xfd000000;  /* str d, [xn|sp], offset */
static const uint32_t stld_pat = 0x3d800000; /* str q, [xn|sp], offset */

/* Generation: fun (fun_addr, res_arg_addresses):
   push x19, x30; sp-=sp_offset; x9=fun_addr; x19=res/arg_addrs
   x10=mem[x19,<offset>]; (arg_reg=mem[x10](or addr of blk copy on the stack)
                          or x10=mem[x10] or x13=addr of blk copy on the stack;
                             mem[sp,sp_offset]=x10|x13) ...
   call fun_addr; sp+=offset
   x10=mem[x19,<offset>]; res_reg=mem[x10]; ...
   pop x19, x30; ret x30. */
void *_MIR_get_ff_call (MIR_context_t ctx, size_t nres, MIR_type_t *res_types, size_t nargs,
                        _MIR_arg_desc_t *arg_descs, size_t arg_vars_num MIR_UNUSED) {
  static const uint32_t prolog[] = {
    0xa9bf7bf3, /* stp x19,x30,[sp, -16]! */
    0xd10003ff, /* sub sp,sp,<sp_offset> */
    0xaa0003e9, /* mov x9,x0   # fun addr */
    0xaa0103f3, /* mov x19, x1 # result/arg addresses */
  };
  static const uint32_t call_end[] = {
    0xd63f0120, /* blr  x9	   */
    0x910003ff, /* add sp,sp,<sp_offset> */
  };
  static const uint32_t epilog[] = {
    0xa8c17bf3, /* ldp x19,x30,[sp],16 */
    0xd65f03c0, /* ret x30 */
  };
  MIR_type_t type;
  uint32_t n_xregs = 0, n_vregs = 0, sp_offset = 0, blk_offset = 0, pat, offset_imm, scale;
  uint32_t sp = 31, addr_reg, qwords;
  const uint32_t temp_reg = 10; /* x10 */
  VARR (uint8_t) * code;
  void *res;

  VARR_CREATE (uint8_t, code, ctx->alloc, 128);
  mir_assert (__SIZEOF_LONG_DOUBLE__ == 8 || __SIZEOF_LONG_DOUBLE__ == 16);
  for (size_t i = 0; i < nargs; i++) { /* calculate offset for blk params */
#if defined(__APPLE__)                 /* all varargs are passed on stack */
    if (i == arg_vars_num) n_xregs = n_vregs = 8;
#endif
    type = arg_descs[i].type;
    if ((MIR_T_I8 <= type && type <= MIR_T_U64) || type == MIR_T_P || MIR_all_blk_type_p (type)) {
      if (MIR_blk_type_p (type) && (qwords = (arg_descs[i].size + 7) / 8) <= 2) {
        if (n_xregs + qwords > 8) blk_offset += qwords * 8;
        n_xregs += qwords;
      } else {
        if (n_xregs++ >= 8) blk_offset += 8;
      }
    } else if (type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD) {
      if (n_vregs++ >= 8) blk_offset += type == MIR_T_LD && __SIZEOF_LONG_DOUBLE__ == 16 ? 16 : 8;
    } else {
      MIR_get_error_func (ctx) (MIR_call_op_error, "wrong type of arg value");
    }
  }
  blk_offset = (blk_offset + 15) / 16 * 16;
  push_insns (code, prolog, sizeof (prolog));
  n_xregs = n_vregs = 0;
  for (size_t i = 0; i < nargs; i++) { /* args */
#if defined(__APPLE__)                 /* all varargs are passed on stack */
    if (i == arg_vars_num) n_xregs = n_vregs = 8;
#endif
    type = arg_descs[i].type;
    scale = type == MIR_T_F ? 2 : type == MIR_T_LD && __SIZEOF_LONG_DOUBLE__ == 16 ? 4 : 3;
    offset_imm = (((i + nres) * sizeof (long double) << 10)) >> scale;
    if (MIR_blk_type_p (type)) {
      qwords = (arg_descs[i].size + 7) / 8;
      if (qwords <= 2) {
        addr_reg = 13;
        pat = ld_pat | offset_imm | addr_reg;
        push_insns (code, &pat, sizeof (pat));
        if (n_xregs + qwords <= 8) {
          for (uint32_t n = 0; n < qwords; n++) {
            pat = gen_ld_pat | (((n * 8) >> scale) << 10) | (n_xregs + n) | (addr_reg << 5);
            push_insns (code, &pat, sizeof (pat));
          }
        } else {
          for (uint32_t n = 0; n < qwords; n++) {
            pat = gen_ld_pat | (((n * 8) >> scale) << 10) | temp_reg | (addr_reg << 5);
            push_insns (code, &pat, sizeof (pat));
            pat = st_pat | ((sp_offset >> scale) << 10) | temp_reg | (sp << 5);
            push_insns (code, &pat, sizeof (pat));
            sp_offset += 8;
          }
        }
        n_xregs += qwords;
      } else {
        addr_reg = n_xregs < 8 ? n_xregs : 13;
        gen_blk_mov (code, blk_offset, (i + nres) * sizeof (long double), qwords, addr_reg);
        blk_offset += qwords * 8;
        if (n_xregs++ >= 8) {
          pat = st_pat | ((sp_offset >> scale) << 10) | addr_reg | (sp << 5);
          push_insns (code, &pat, sizeof (pat));
          sp_offset += 8;
        }
      }
    } else if ((MIR_T_I8 <= type && type <= MIR_T_U64) || type == MIR_T_P || type == MIR_T_RBLK) {
      if (type == MIR_T_RBLK && i == 0) {
        pat = ld_pat | offset_imm | 8; /* x8 - hidden result address */
      } else if (n_xregs < 8) {
        pat = ld_pat | offset_imm | n_xregs++;
      } else {
        pat = ld_pat | offset_imm | temp_reg;
        push_insns (code, &pat, sizeof (pat));
        pat = st_pat | ((sp_offset >> scale) << 10) | temp_reg | (sp << 5);
        sp_offset += 8;
      }
      push_insns (code, &pat, sizeof (pat));
    } else if (type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD) {
      pat = type == MIR_T_F                                  ? lds_pat
            : type == MIR_T_D || __SIZEOF_LONG_DOUBLE__ == 8 ? ldd_pat
                                                             : ldld_pat;
      if (n_vregs < 8) {
        pat |= offset_imm | n_vregs++;
      } else {
        if (type == MIR_T_LD && __SIZEOF_LONG_DOUBLE__ == 16) sp_offset = (sp_offset + 15) % 16;
        pat |= offset_imm | temp_reg;
        push_insns (code, &pat, sizeof (pat));
        pat = type == MIR_T_F                                  ? sts_pat
              : type == MIR_T_D || __SIZEOF_LONG_DOUBLE__ == 8 ? std_pat
                                                               : stld_pat;
        pat |= ((sp_offset >> scale) << 10) | temp_reg | (sp << 5);
        sp_offset += type == MIR_T_LD && __SIZEOF_LONG_DOUBLE__ == 16 ? 16 : 8;
      }
      push_insns (code, &pat, sizeof (pat));
    } else {
      MIR_get_error_func (ctx) (MIR_call_op_error, "wrong type of arg value");
    }
  }
  sp_offset = (sp_offset + 15) / 16 * 16;
  blk_offset = (blk_offset + 15) / 16 * 16;
  if (blk_offset != 0) sp_offset = blk_offset;
  mir_assert (sp_offset < (1 << 12));
  ((uint32_t *) VARR_ADDR (uint8_t, code))[1] |= sp_offset << 10; /* sub sp,sp,<offset> */
  push_insns (code, call_end, sizeof (call_end));
  ((uint32_t *) (VARR_ADDR (uint8_t, code) + VARR_LENGTH (uint8_t, code)))[-1] |= sp_offset << 10;
  n_xregs = n_vregs = 0;
  for (size_t i = 0; i < nres; i++) { /* results */
    offset_imm = i * sizeof (long double) << 10;
    if (((MIR_T_I8 <= res_types[i] && res_types[i] <= MIR_T_U64) || res_types[i] == MIR_T_P)
        && n_xregs < 8) {
      offset_imm >>= 3;
      pat = st_pat | offset_imm | n_xregs++ | (19 << 5);
      push_insns (code, &pat, sizeof (pat));
    } else if ((res_types[i] == MIR_T_F || res_types[i] == MIR_T_D || res_types[i] == MIR_T_LD)
               && n_vregs < 8) {
      offset_imm >>= res_types[i] == MIR_T_F                                  ? 2
                     : res_types[i] == MIR_T_D || __SIZEOF_LONG_DOUBLE__ == 8 ? 3
                                                                              : 4;
      pat = res_types[i] == MIR_T_F                                  ? sts_pat
            : res_types[i] == MIR_T_D || __SIZEOF_LONG_DOUBLE__ == 8 ? std_pat
                                                                     : stld_pat;
      pat |= offset_imm | n_vregs++ | (19 << 5);
      push_insns (code, &pat, sizeof (pat));
    } else {
      MIR_get_error_func (ctx) (MIR_ret_error,
                                "aarch64 can not handle this combination of return values");
    }
  }
  push_insns (code, epilog, sizeof (epilog));
  res = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
#if 0
  if (getenv ("MIR_code_dump") != NULL)
    _MIR_dump_code ("ffi:", VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
#endif
  VARR_DESTROY (uint8_t, code);
  return res;
}

/* Transform C call to call of void handler (MIR_context_t ctx, MIR_item_t func_item,
                                             va_list va, MIR_val_t *results) */
void *_MIR_get_interp_shim (MIR_context_t ctx, MIR_item_t func_item, void *handler) {
  static const uint32_t save_x19_pat = 0xf81f0ff3; /* str x19, [sp,-16]! */
  static const uint32_t prepare_pat[] = {
#if !defined(__APPLE__)
    0xd10083ff, /* sub sp, sp, 32 # allocate va_list */
    0x910003ea, /* mov x10, sp # va_list addr         */
    0xb9001949, /* str w9,[x10, 24] # va_list.gr_offs */
    0x12800fe9, /* mov w9, #-128 # vr_offs */
    0xb9001d49, /* str w9,[x10, 28]  #va_list.vr_offs */
    0x9103c3e9, /* add x9, sp, #240 # gr_top */
    0xf9000549, /* str x9,[x10, 8] # va_list.gr_top */
    0x91004129, /* add x9, x9, #16 # stack */
    0xf9000149, /* str x9,[x10] # valist.stack */
    0x910283e9, /* add x9, sp, #160 # vr_top*/
    0xf9000949, /* str x9,[x10, 16] # va_list.vr_top */
    0xaa0a03e2, /* mov x2, x10 # va arg  */
#endif
    0xd2800009, /* mov x9, <(nres+1)*16> */
    0xcb2963ff, /* sub sp, sp, x9 # reserve results and place for saved lr */
#if defined(__APPLE__)
    0x910023e3, /* add x3, sp, 8 # results arg */
#else
    0x910043e3,                                      /* add x3, sp, 16 # results arg */
#endif
    0xaa0303f3, /* mov x19, x3 # results */
    0xf90003fe, /* str x30, [sp] # save lr */
  };
  static const uint32_t shim_end[]
    = { 0xf94003fe, /* ldr x30, [sp] */
        0xd2800009, /* mov x9, 240+(nres+1)*16 or APPLE: (nres * 8 + 8 + 15)/16*16 + sp_offset */
#if defined(__APPLE__)
        0xf94003f3, /* ldr x19, [sp, <(nres * 8 + 8 + 15)/16*16>] */
#endif
        0x8b2963ff, /* add sp, sp, x9 */
#if !defined(__APPLE__)
        0xf84107f3, /* ldr x19, sp, 16 */
#endif
        0xd65f03c0, /* ret x30 */
      };
  uint32_t pat, imm, n_xregs, n_vregs, offset, offset_imm;
  MIR_func_t func = func_item->u.func;
  uint32_t nres = func->nres;
  MIR_type_t *results = func->res_types;
  VARR (uint8_t) * code;
  void *res;

  VARR_CREATE (uint8_t, code, ctx->alloc, 128);
#if defined(__APPLE__)
  int stack_arg_sp_offset, sp_offset, scale;
  uint32_t qwords, sp = 31;
  uint32_t base_reg_mask = ~(uint32_t) (0x3f << 5);
  static const uint32_t temp_reg = 10; /* x10 */
  MIR_type_t type;
  static const uint32_t add_x2_sp = 0x910003e2; /* add x2, sp, X*/
  static const uint32_t arg_mov_start_pat[] = {
    0x910003e9, /* mov x9,sp */
    0xd10003ff, /* sub sp, sp, <frame size:10-21> # non-varg */
  };

  assert (__SIZEOF_LONG_DOUBLE__ == 8);
  push_insns (code, arg_mov_start_pat, sizeof (arg_mov_start_pat));
  sp_offset = 0;
  for (size_t i = 0; i < func->nargs; i++) { /* args */
    type = VARR_GET (MIR_var_t, func->vars, i).type;
    if (MIR_blk_type_p (type)
        && (qwords = (VARR_GET (MIR_var_t, func->vars, i).size + 7) / 8) <= 2) {
      /* passing by one or two qwords */
      sp_offset += 8 * qwords;
      continue;
    }
    sp_offset += 8;
  }
  imm = sp_offset % 16;
  sp_offset = imm == 0 ? 0 : 8;
  stack_arg_sp_offset = 0;
  n_xregs = n_vregs = 0;
  for (size_t i = 0; i < func->nargs; i++) { /* args */
    type = VARR_GET (MIR_var_t, func->vars, i).type;
    scale = type == MIR_T_F ? 2 : 3;
    if (MIR_blk_type_p (type)
        && (qwords = (VARR_GET (MIR_var_t, func->vars, i).size + 7) / 8) <= 2) {
      /* passing by one or two qwords */
      if (n_xregs + qwords
          <= 8) { /* passed by hard regs: str xreg, offset[sp]; str xreg, offset+8[sp] */
        for (uint32_t n = 0; n < qwords; n++) {
          pat = st_pat | ((sp_offset >> scale) << 10) | n_xregs++ | (sp << 5);
          sp_offset += 8;
          push_insns (code, &pat, sizeof (pat));
        }
      } else {
        /* passed on stack: ldr t, stack_arg_offset[x9]; st t, offset[sp];
                            ldr t, stack_arg_offset+8[x9]; st t, offset+8[sp]: */
        for (int n = 0; n < qwords; n++) {
          pat
            = (ld_pat & base_reg_mask) | (stack_arg_sp_offset >> scale) << 10 | temp_reg | (9 << 5);
          push_insns (code, &pat, sizeof (pat));
          pat = st_pat | ((sp_offset >> scale) << 10) | temp_reg | (sp << 5);
          push_insns (code, &pat, sizeof (pat));
          stack_arg_sp_offset += 8;
          sp_offset += 8;
        }
      }
      continue;
    }
    if ((MIR_T_I8 <= type && type <= MIR_T_U64) || type == MIR_T_P || type == MIR_T_RBLK
        || MIR_blk_type_p (type)) {       /* including address for long blocks */
      if (type == MIR_T_RBLK && i == 0) { /* x8 - hidden result address */
        pat = st_pat | ((sp_offset >> scale) << 10) | 8 | (sp << 5);
      } else if (n_xregs < 8) { /* str xreg, sp_offset[sp]  */
        pat = st_pat | ((sp_offset >> scale) << 10) | n_xregs++ | (sp << 5);
      } else { /* ldr t, stack_arg_offset[x9]; st t, sp_offset[sp]: */
        pat
          = (ld_pat & base_reg_mask) | ((stack_arg_sp_offset >> scale) << 10) | temp_reg | (9 << 5);
        push_insns (code, &pat, sizeof (pat));
        pat = st_pat | ((sp_offset >> scale) << 10) | temp_reg | (sp << 5);
        stack_arg_sp_offset += 8;
      }
      sp_offset += 8;
      push_insns (code, &pat, sizeof (pat));
    } else if (type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD) {
      if (n_vregs < 8) { /* st[s|d] vreg, sp_offset[sp]  */
        pat = (type == MIR_T_F ? sts_pat : std_pat) | ((sp_offset >> scale) << 10) | n_vregs++
              | (sp << 5);
        sp_offset += 8;
      } else {
        pat = ((type == MIR_T_F ? lds_pat : ldd_pat) & base_reg_mask) | (9 << 5);
        pat |= stack_arg_sp_offset | temp_reg;
        push_insns (code, &pat, sizeof (pat));
        pat = (type == MIR_T_F ? sts_pat : std_pat) | ((sp_offset >> scale) << 10) | temp_reg
              | (sp << 5);
        stack_arg_sp_offset += 8;
        sp_offset += 8;
      }
      push_insns (code, &pat, sizeof (pat));
    } else {
      MIR_get_error_func (ctx) (MIR_call_op_error, "wrong type of arg value");
    }
  }
  pat = add_x2_sp | (imm << 10);
  push_insns (code, &pat, sizeof (pat));
  sp_offset = (sp_offset + 15) / 16 * 16;
  ((uint32_t *) VARR_ADDR (uint8_t, code))[1] |= sp_offset << 10;
  push_insns (code, &save_x19_pat, sizeof (save_x19_pat));
#else
  static const uint32_t set_gr_offs = 0x128007e9;    /* mov w9, #-64 # gr_offs */
  static const uint32_t set_x8_gr_offs = 0x128008e9; /* mov w9, #-72 # gr_offs */
  int x8_res_p = func->nargs != 0 && VARR_GET (MIR_var_t, func->vars, 0).type == MIR_T_RBLK;
  push_insns (code, &save_x19_pat, sizeof (save_x19_pat));
  push_insns (code, save_insns, sizeof (save_insns));
  if (x8_res_p)
    push_insns (code, &set_x8_gr_offs, sizeof (set_x8_gr_offs));
  else
    push_insns (code, &set_gr_offs, sizeof (set_gr_offs));
#endif
  push_insns (code, prepare_pat, sizeof (prepare_pat));
  imm = (nres * sizeof (MIR_val_t) + 8 + 15) / 16 * 16; /* results + saved x30 aligned to 16 */
  mir_assert (imm < (1 << 16));
  ((uint32_t *) (VARR_ADDR (uint8_t, code) + VARR_LENGTH (uint8_t, code)))[-5] |= imm << 5;
  gen_mov_addr (code, 0, ctx);       /* mov x0, ctx */
  gen_mov_addr (code, 1, func_item); /* mov x1, func_item */
  gen_call_addr (code, NULL, 9, handler);
  /* move results: */
  n_xregs = n_vregs = offset = 0;
  for (uint32_t i = 0; i < nres; i++) {
    if ((results[i] == MIR_T_F || results[i] == MIR_T_D || results[i] == MIR_T_LD) && n_vregs < 8) {
      pat = results[i] == MIR_T_F                                  ? lds_pat
            : results[i] == MIR_T_D || __SIZEOF_LONG_DOUBLE__ == 8 ? ldd_pat
                                                                   : ldld_pat;
      pat |= n_vregs;
      n_vregs++;
    } else if (n_xregs < 8) {  // ??? ltp use
      pat = ld_pat | n_xregs;
      n_xregs++;
    } else {
      MIR_get_error_func (ctx) (MIR_ret_error,
                                "aarch64 can not handle this combination of return values");
    }
    offset_imm = offset >> (results[i] == MIR_T_F                                    ? 2
                            : results[i] == MIR_T_LD && __SIZEOF_LONG_DOUBLE__ == 16 ? 4
                                                                                     : 3);
    mir_assert (offset_imm < (1 << 12));
    pat |= offset_imm << 10;
    push_insns (code, &pat, sizeof (pat));
    offset += sizeof (MIR_val_t);
  }
  push_insns (code, shim_end, sizeof (shim_end));
#if defined(__APPLE__)
  assert (imm % 8 == 0);
  ((uint32_t *) (VARR_ADDR (uint8_t, code) + VARR_LENGTH (uint8_t, code)))[-3] |= imm << 7;
  imm += sp_offset + 16;
#else
  imm = 240 + (nres + 1) * 16;
#endif
  mir_assert (imm < (1 << 16));
  ((uint32_t *) (VARR_ADDR (uint8_t, code) + VARR_LENGTH (uint8_t, code)))[-4] |= imm << 5;
  res = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
#if 0
  if (getenv ("MIR_code_dump") != NULL)
    _MIR_dump_code (func->name, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
#endif
  VARR_DESTROY (uint8_t, code);
  return res;
}

/* Save x0,x1; x0=ctx; x1=called_func; x10=hook_address;goto wrap_end. */
void *_MIR_get_wrapper (MIR_context_t ctx, MIR_item_t called_func, void *hook_address) {
  static const uint32_t save_insn = 0xa9bf07e0; /* stp R0, R1, [SP, #-16]! */
  static const uint32_t jmp_pat = 0x14000000;   /* jmp */
  uint32_t insn;
  uint8_t *base_addr, *curr_addr, *res_code = NULL;
  size_t len = 5 * 4; /* initial len */
  VARR (uint8_t) * code;

  VARR_CREATE (uint8_t, code, ctx->alloc, 128);
  for (;;) { /* dealing with moving code to another page as the immediate call is pc relative */
    curr_addr = base_addr = _MIR_get_new_code_addr (ctx, len);
    if (curr_addr == NULL) break;
    VARR_TRUNC (uint8_t, code, 0);
    push_insns (code, &save_insn, sizeof (save_insn));
    curr_addr += 4;
    curr_addr += gen_mov_addr (code, 0, ctx);           /*mov x0,ctx  	   */
    curr_addr += gen_mov_addr (code, 1, called_func);   /*mov x1,called_func */
    curr_addr += gen_mov_addr (code, 10, hook_address); /*mov x10,hook_address */
    int64_t offset = (uint32_t *) wrapper_end_addr - (uint32_t *) curr_addr;
    mir_assert (-(int64_t) MAX_BR_OFFSET <= offset && offset < (int64_t) MAX_BR_OFFSET);
    insn = jmp_pat | ((uint32_t) offset & BR_OFFSET_MASK);
    push_insns (code, &insn, sizeof (insn));
    len = VARR_LENGTH (uint8_t, code);
    res_code = _MIR_publish_code_by_addr (ctx, base_addr, VARR_ADDR (uint8_t, code), len);
    if (res_code != NULL) break;
  }
  VARR_DESTROY (uint8_t, code);
  return res_code;
}

void *_MIR_get_wrapper_end (MIR_context_t ctx) {
  static const uint32_t wrap_end[] = {
    0xa9bf7bfd, /* stp R29, R30, [SP, #-16]! */
    0xa9bf1fe6, /* stp R6, R7, [SP, #-16]! */
    0xa9bf17e4, /* stp R4, R5, [SP, #-16]! */
    0xa9bf0fe2, /* stp R2, R3, [SP, #-16]! */
    0xd10043ff, /* sub SP, SP, #16 */
    0xf90007e8, /* str x8, [SP, #8] */
    0xadbf1fe6, /* stp Q6, Q7, [SP, #-32]! */
    0xadbf17e4, /* stp Q4, Q5, [SP, #-32]! */
    0xadbf0fe2, /* stp Q2, Q3, [SP, #-32]! */
    0xadbf07e0, /* stp Q0, Q1, [SP, #-32]! */
    0xd63f0140, /* call *x10 */
    0xaa0003e9, /* mov x9, x0 */
    0xacc107e0, /* ldp Q0, Q1, SP, #32 */
    0xacc10fe2, /* ldp Q2, Q3, SP, #32 */
    0xacc117e4, /* ldp Q4, Q5, SP, #32 */
    0xacc11fe6, /* ldp Q6, Q7, SP, #32 */
    0xf94007e8, /* ldr x8, [SP, #8] */
    0x910043ff, /* add SP, SP, #16 */
    0xa8c10fe2, /* ldp R2, R3, SP, #16 */
    0xa8c117e4, /* ldp R4, R5, SP, #16 */
    0xa8c11fe6, /* ldp R6, R7, SP, #16 */
    0xa8c17bfd, /* ldp R29, R30, SP, #16 */
    0xa8c107e0, /* ldp R0, R1, SP, #16 */
    0xd61f0120, /* br x9 */
  };
  uint8_t *res_code = NULL;
  VARR (uint8_t) * code;
  size_t len;

  VARR_CREATE (uint8_t, code, ctx->alloc, 128);
  push_insns (code, wrap_end, sizeof (wrap_end));
  len = VARR_LENGTH (uint8_t, code);
  res_code = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, code), len);
  VARR_DESTROY (uint8_t, code);
  return res_code;
}

/* r9=<bb_version>; (b|br) handler  ??? mutex free */
void *_MIR_get_bb_thunk (MIR_context_t ctx, void *bb_version, void *handler) {
  /* maximal size thunk -- see _MIR_redirect_thunk */
  uint32_t pat[5] = {TARGET_NOP, TARGET_NOP, TARGET_NOP, TARGET_NOP, TARGET_NOP};
  void *res;
  size_t offset;
  VARR (uint8_t) * code;

  VARR_CREATE (uint8_t, code, ctx->alloc, 64);
  offset = gen_mov_addr (code, 9, bb_version); /* x9 = bb_version */
  push_insns (code, pat, sizeof (pat));
  res = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
  _MIR_redirect_thunk (ctx, (uint8_t *) res + offset, handler);
#if 0
  if (getenv ("MIR_code_dump") != NULL)
    _MIR_dump_code ("bb thunk:", res, VARR_LENGTH (uint8_t, code));
#endif
  VARR_DESTROY (uint8_t, code);
  return res;
}

/* change to (b|br) to */
void _MIR_replace_bb_thunk (MIR_context_t ctx, void *thunk, void *to) {
  _MIR_redirect_thunk (ctx, thunk, to);
}

static const uint32_t save_fplr = 0xa9bf7bfd;    /* stp R29, R30, [SP, #-16]! */
static const uint32_t restore_fplr = 0xa8c17bfd; /* ldp R29, R30, SP, #16 */

static const uint32_t save_insns2[] = {
  /* save r10-r18,v16-v31: should be used only right after save_insn */
  0xf90043ea, /* str R10, [SP, #128]  */
  0xa9bf4bf1, /* stp R17, R18, [SP, #-16]! */
  0xa9bf43ef, /* stp R15, R16, [SP, #-16]! */
  0xa9bf3bed, /* stp R13, R14, [SP, #-16]! */
  0xa9bf33eb, /* stp R11, R12, [SP, #-16]! */
  0xadbf7ffe, /* stp Q30, Q31, [SP, #-32]! */
  0xadbf77fc, /* stp Q28, Q29, [SP, #-32]! */
  0xadbf6ffa, /* stp Q26, Q27, [SP, #-32]! */
  0xadbf67f8, /* stp Q24, Q25, [SP, #-32]! */
  0xadbf5ff6, /* stp Q22, Q23, [SP, #-32]! */
  0xadbf57f4, /* stp Q20, Q21, [SP, #-32]! */
  0xadbf4ff2, /* stp Q18, Q19, [SP, #-32]! */
  0xadbf47f0, /* stp Q16, Q17, [SP, #-32]! */
};
static const uint32_t restore_insns2[] = {
  /* restore r10-r18,v16-v32: should be used only right before restore_insns */
  0xacc147f0, /* ldp Q16, Q17, SP, #32 */
  0xacc14ff2, /* ldp Q18, Q19, SP, #32 */
  0xacc157f4, /* ldp Q20, Q21, SP, #32 */
  0xacc15ff6, /* ldp Q22, Q23, SP, #32 */
  0xacc167f8, /* ldp Q24, Q25, SP, #32 */
  0xacc16ffa, /* ldp Q26, Q27, SP, #32 */
  0xacc177fc, /* ldp Q28, Q29, SP, #32 */
  0xacc17ffe, /* ldp Q30, Q31, SP, #32 */
  0xa8c133eb, /* ldp R11, R12, SP, #16 */
  0xa8c13bed, /* ldp R13, R14, SP, #16 */
  0xa8c143ef, /* ldp R15, R16, SP, #16 */
  0xa8c14bf1, /* ldp R17, R18, SP, #16 */
  0xf94043ea, /* ldr R10, [SP, #128]  */
};

/* save all clobbered regs but 9; r9 = call hook_address (data, r9); restore regs; br r9
   r9 is a generator temp reg which is not used across bb borders. */
void *_MIR_get_bb_wrapper (MIR_context_t ctx, void *data, void *hook_address) {
  static const uint32_t wrap_end = 0xd61f0120; /* br   x9			   */
  static const uint32_t call_pat[] = {
    0xaa0903e1, /* mov x1,x9 */
    0xd63f0140, /* blr  x10 */
    0xaa0003e9, /* mov x9,x0 */
  };
  void *res;
  VARR (uint8_t) * code;

  VARR_CREATE (uint8_t, code, ctx->alloc, 128);
  push_insns (code, &save_fplr, sizeof (save_fplr));
  push_insns (code, save_insns, sizeof (save_insns));
  push_insns (code, save_insns2, sizeof (save_insns2));
  gen_mov_addr (code, 10, hook_address); /* x10 = hook_address */
  gen_mov_addr (code, 0, data);          /* x0 = data */
  push_insns (code, call_pat, sizeof (call_pat));
  push_insns (code, restore_insns2, sizeof (restore_insns2));
  push_insns (code, restore_insns, sizeof (restore_insns));
  push_insns (code, &restore_fplr, sizeof (restore_fplr));
  push_insns (code, &wrap_end, sizeof (wrap_end));
  res = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
#if 0
  if (getenv ("MIR_code_dump") != NULL)
    _MIR_dump_code ("bb wrapper:", VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
#endif
  VARR_DESTROY (uint8_t, code);
  return res;
}
