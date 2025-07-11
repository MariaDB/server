/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include "mir-x86_64.h"

/* RBLK args are always passed by address.
   BLK0 first is copied on the caller stack and passed implicitly.
   BLK1 is passed in general regs
   BLK2 is passed in fp regs
   BLK3 is passed in gpr and then fpr
   BLK4 is passed in fpr and then gpr
   If there are no enough regs, they work as BLK.
   Windows: small BLKs (<= 8 bytes) are passed by value;
            all other BLKs is always passed by pointer as regular int arg.  */

#define VA_LIST_IS_ARRAY_P 1

void *_MIR_get_bstart_builtin (MIR_context_t ctx) {
  static const uint8_t bstart_code[] = {
    0x48, 0x8d, 0x44, 0x24, 0x08, /* rax = rsp + 8 (lea) */
    0xc3,                         /* ret */
  };
  return _MIR_publish_code (ctx, bstart_code, sizeof (bstart_code));
}
void *_MIR_get_bend_builtin (MIR_context_t ctx) {
  static const uint8_t bend_code[] = {
#ifndef _WIN32
    0x48, 0x8b, 0x04, 0x24, /* rax = (rsp) */
    0x48, 0x89, 0xfc,       /* rsp = rdi */
    0xff, 0xe0,             /* jmp *rax */
#else
    0x48, 0x8b, 0x04, 0x24, /* rax = (rsp) */
    0x48, 0x89, 0xcc,       /* rsp = rcx */
    0xff, 0xe0,             /* jmp *rax */
#endif
  };
  return _MIR_publish_code (ctx, bend_code, sizeof (bend_code));
}

#ifndef _WIN32
struct x86_64_va_list {
  uint32_t gp_offset, fp_offset;
  uint64_t *overflow_arg_area, *reg_save_area;
};

void *va_arg_builtin (void *p, uint64_t t) {
  struct x86_64_va_list *va = p;
  MIR_type_t type = t;
  int fp_p = type == MIR_T_F || type == MIR_T_D;
  void *a;

  if (fp_p && va->fp_offset <= 160) {
    a = (char *) va->reg_save_area + va->fp_offset;
    va->fp_offset += 16;
  } else if (!fp_p && type != MIR_T_LD && va->gp_offset <= 40) {
    a = (char *) va->reg_save_area + va->gp_offset;
    va->gp_offset += 8;
  } else {
    a = va->overflow_arg_area;
    va->overflow_arg_area += type == MIR_T_LD ? 2 : 1;
  }
  return a;
}

void va_block_arg_builtin (void *res, void *p, size_t s, uint64_t ncase) {
  struct x86_64_va_list *va = p;
  size_t size = ((s + 7) / 8) * 8;
  void *a = va->overflow_arg_area;
  union {
    uint64_t i;
    double d;
  } u[2];

  switch (ncase) {
  case 1:
    if (va->gp_offset + size > 48) break;
    u[0].i = *(uint64_t *) ((char *) va->reg_save_area + va->gp_offset);
    va->gp_offset += 8;
    if (size > 8) {
      u[1].i = *(uint64_t *) ((char *) va->reg_save_area + va->gp_offset);
      va->gp_offset += 8;
    }
    if (res != NULL) memcpy (res, &u, s);
    return;
  case 2:
    u[0].d = *(double *) ((char *) va->reg_save_area + va->fp_offset);
    va->fp_offset += 16;
    if (size > 8) {
      u[1].d = *(double *) ((char *) va->reg_save_area + va->fp_offset);
      va->fp_offset += 16;
    }
    if (res != NULL) memcpy (res, &u, s);
    return;
  case 3:
  case 4:
    if (va->fp_offset > 160 || va->gp_offset > 40) break;
    if (ncase == 3) {
      u[0].i = *(uint64_t *) ((char *) va->reg_save_area + va->gp_offset);
      u[1].d = *(double *) ((char *) va->reg_save_area + va->fp_offset);
    } else {
      u[0].d = *(double *) ((char *) va->reg_save_area + va->fp_offset);
      u[1].i = *(uint64_t *) ((char *) va->reg_save_area + va->gp_offset);
    }
    va->fp_offset += 8;
    va->gp_offset += 8;
    if (res != NULL) memcpy (res, &u, s);
    return;
  default: break;
  }
  if (res != NULL) memcpy (res, a, s);
  va->overflow_arg_area += size / 8;
}

void va_start_interp_builtin (MIR_context_t ctx MIR_UNUSED, void *p, void *a) {
  struct x86_64_va_list *va = p;
  va_list *vap = a;

  assert (sizeof (struct x86_64_va_list) == sizeof (va_list));
  *va = *(struct x86_64_va_list *) vap;
}

#else

struct x86_64_va_list {
  uint64_t *arg_area;
};

void *va_arg_builtin (void *p, uint64_t t) {
  struct x86_64_va_list *va = p;
  void *a = va->arg_area;
  va->arg_area++;
  return a;
}

void va_block_arg_builtin (void *res, void *p, size_t s, uint64_t ncase) {
  struct x86_64_va_list *va = p;
  void *a = s <= 8 ? va->arg_area : *(void **) va->arg_area; /* pass by pointer */
  if (res != NULL) memcpy (res, a, s);
  va->arg_area++;
}

void va_start_interp_builtin (MIR_context_t ctx, void *p, void *a) {
  struct x86_64_va_list **va = p;
  va_list *vap = a;

  assert (sizeof (struct x86_64_va_list) == sizeof (va_list));
  *va = (struct x86_64_va_list *) vap;
}

#endif

void va_end_interp_builtin (MIR_context_t ctx MIR_UNUSED, void *p MIR_UNUSED) {}

static const uint8_t short_jmp_pattern[] = {
  0xe9, 0, 0, 0, 0,         /* 0x0: jmp rel32 */
  0,    0, 0, 0, 0, 0, 0, 0 /* 0x5: abs address holder */
};
static const uint8_t long_jmp_pattern[] = {
  0x49, 0xbb, 0,    0, 0, 0, 0, 0, 0, 0, /* 0x0: movabsq 0, r11 */
  0x41, 0xff, 0xe3,                      /* 0xa: jmpq   *%r11 */
};

/* r11=<address to go to>; jump *r11  */
void *_MIR_get_thunk (MIR_context_t ctx) {
  void *res;
  assert (sizeof (short_jmp_pattern) == sizeof (long_jmp_pattern));
  res = _MIR_publish_code (ctx, short_jmp_pattern, sizeof (short_jmp_pattern));
  return res;
}

void *_MIR_get_thunk_addr (MIR_context_t ctx MIR_UNUSED, void *thunk) {
  void *addr;
  int short_p = *(unsigned char *) thunk == 0xe9;
  memcpy ((char *) &addr, (char *) thunk + (short_p ? 5 : 2), sizeof (addr));
  return addr;
}

void _MIR_redirect_thunk (MIR_context_t ctx, void *thunk, void *to) {
  int64_t disp = (char *) to - ((char *) thunk + 5);
  int short_p = INT32_MIN <= disp && disp <= INT32_MAX;
  uint8_t pattern[sizeof (short_jmp_pattern)];
  if (short_p) {
    memcpy (pattern, short_jmp_pattern, sizeof (short_jmp_pattern));
    memcpy (pattern + 1, &disp, 4); /* little endian */
    memcpy (pattern + 5, &to, 8);
  } else {
    memcpy (pattern, long_jmp_pattern, sizeof (long_jmp_pattern));
    memcpy (pattern + 2, &to, 8);
  }
  _MIR_change_code (ctx, thunk, pattern, sizeof (short_jmp_pattern));
}

static const uint8_t save_pat[] = {
#ifndef _WIN32
  0x48, 0x81, 0xec, 0x80, 0,    0,    0, /*sub    $0x88,%rsp		   */
  0xf3, 0x0f, 0x7f, 0x04, 0x24,          /*movdqu %xmm0,(%rsp)		   */
  0xf3, 0x0f, 0x7f, 0x4c, 0x24, 0x10,    /*movdqu %xmm1,0x10(%rsp)	   */
  0xf3, 0x0f, 0x7f, 0x54, 0x24, 0x20,    /*movdqu %xmm2,0x20(%rsp)	   */
  0xf3, 0x0f, 0x7f, 0x5c, 0x24, 0x30,    /*movdqu %xmm3,0x30(%rsp)	   */
  0xf3, 0x0f, 0x7f, 0x64, 0x24, 0x40,    /*movdqu %xmm4,0x40(%rsp)	   */
  0xf3, 0x0f, 0x7f, 0x6c, 0x24, 0x50,    /*movdqu %xmm5,0x50(%rsp)	   */
  0xf3, 0x0f, 0x7f, 0x74, 0x24, 0x60,    /*movdqu %xmm6,0x60(%rsp)	   */
  0xf3, 0x0f, 0x7f, 0x7c, 0x24, 0x70,    /*movdqu %xmm7,0x70(%rsp)	   */
  0x41, 0x51,                            /*push   %r9			   */
  0x41, 0x50,                            /*push   %r8			   */
  0x51,                                  /*push   %rcx			   */
  0x52,                                  /*push   %rdx			   */
  0x56,                                  /*push   %rsi			   */
  0x57,                                  /*push   %rdi			   */
#else
  0x48, 0x89, 0x4c, 0x24, 0x08, /*mov  %rcx,0x08(%rsp) */
  0x48, 0x89, 0x54, 0x24, 0x10, /*mov  %rdx,0x10(%rsp) */
  0x4c, 0x89, 0x44, 0x24, 0x18, /*mov  %r8, 0x18(%rsp) */
  0x4c, 0x89, 0x4c, 0x24, 0x20, /*mov  %r9, 0x20(%rsp) */
#endif
};

static const uint8_t restore_pat[] = {
#ifndef _WIN32
  0x5f,                                  /*pop    %rdi			   */
  0x5e,                                  /*pop    %rsi			   */
  0x5a,                                  /*pop    %rdx			   */
  0x59,                                  /*pop    %rcx			   */
  0x41, 0x58,                            /*pop    %r8			   */
  0x41, 0x59,                            /*pop    %r9			   */
  0xf3, 0x0f, 0x6f, 0x04, 0x24,          /*movdqu (%rsp),%xmm0		   */
  0xf3, 0x0f, 0x6f, 0x4c, 0x24, 0x10,    /*movdqu 0x10(%rsp),%xmm1	   */
  0xf3, 0x0f, 0x6f, 0x54, 0x24, 0x20,    /*movdqu 0x20(%rsp),%xmm2	   */
  0xf3, 0x0f, 0x6f, 0x5c, 0x24, 0x30,    /*movdqu 0x30(%rsp),%xmm3	   */
  0xf3, 0x0f, 0x6f, 0x64, 0x24, 0x40,    /*movdqu 0x40(%rsp),%xmm4	   */
  0xf3, 0x0f, 0x6f, 0x6c, 0x24, 0x50,    /*movdqu 0x50(%rsp),%xmm5	   */
  0xf3, 0x0f, 0x6f, 0x74, 0x24, 0x60,    /*movdqu 0x60(%rsp),%xmm6	   */
  0xf3, 0x0f, 0x6f, 0x7c, 0x24, 0x70,    /*movdqu 0x70(%rsp),%xmm7	   */
  0x48, 0x81, 0xc4, 0x80, 0,    0,    0, /*add    $0x80,%rsp		   */
#else
  0x48, 0x8b, 0x4c, 0x24, 0x08,       /*mov  0x08(%rsp),%rcx */
  0x48, 0x8b, 0x54, 0x24, 0x10,       /*mov  0x10(%rsp),%rdx */
  0x4c, 0x8b, 0x44, 0x24, 0x18,       /*mov  0x18(%rsp),%r8  */
  0x4c, 0x8b, 0x4c, 0x24, 0x20,       /*mov  0x20(%rsp),%r9  */
  0xf3, 0x0f, 0x7e, 0x44, 0x24, 0x08, /*movq 0x08(%rsp),%xmm0*/
  0xf3, 0x0f, 0x7e, 0x4c, 0x24, 0x10, /*movq 0x10(%rsp),%xmm1*/
  0xf3, 0x0f, 0x7e, 0x54, 0x24, 0x18, /*movq 0x18(%rsp),%xmm2*/
  0xf3, 0x0f, 0x7e, 0x5c, 0x24, 0x20, /*movq 0x20(%rsp),%xmm3*/
#endif
};

static uint8_t *push_insns (VARR (uint8_t) * insn_varr, const uint8_t *pat, size_t pat_len) {
  for (size_t i = 0; i < pat_len; i++) VARR_PUSH (uint8_t, insn_varr, pat[i]);
  return VARR_ADDR (uint8_t, insn_varr) + VARR_LENGTH (uint8_t, insn_varr) - pat_len;
}

static void gen_mov (VARR (uint8_t) * insn_varr, uint32_t offset, uint32_t reg, int ld_p) {
  static const uint8_t ld_gp_reg[] = {0x48, 0x8b, 0x83, 0, 0, 0, 0 /* mov <offset>(%rbx),%reg */};
  static const uint8_t st_gp_reg[] = {0x48, 0x89, 0x83, 0, 0, 0, 0 /* mov %reg,<offset>(%rbx) */};
  uint8_t *addr = push_insns (insn_varr, ld_p ? ld_gp_reg : st_gp_reg,
                              ld_p ? sizeof (ld_gp_reg) : sizeof (st_gp_reg));
  memcpy (addr + 3, &offset, sizeof (uint32_t));
  assert (reg <= 15);
  addr[0] |= (reg >> 1) & 4;
  addr[2] |= (reg & 7) << 3;
}

static void gen_mov2 (VARR (uint8_t) * insn_varr, uint32_t offset, uint32_t reg, int ld_p) {
  static const uint8_t ld_gp_reg[] = {0x49, 0x8b, 0x44, 0x24, 0 /* mov <offset>(%r12),%reg */};
  static const uint8_t st_gp_reg[] = {0x49, 0x89, 0x44, 0x24, 0 /* mov %reg,<offset>(%r12) */};
  uint8_t *addr = push_insns (insn_varr, ld_p ? ld_gp_reg : st_gp_reg,
                              ld_p ? sizeof (ld_gp_reg) : sizeof (st_gp_reg));
  addr[4] = offset;
  assert (reg <= 15);
  addr[0] |= (reg >> 1) & 4;
  addr[2] |= (reg & 7) << 3;
}

static void gen_blk_mov (VARR (uint8_t) * insn_varr, uint32_t offset, uint32_t addr_offset,
                         uint32_t qwords) {
  static const uint8_t blk_mov_pat[] = {
    /*0:*/ 0x4c,  0x8b, 0xa3, 0,    0, 0, 0,    /*mov <addr_offset>(%rbx),%r12*/
    /*7:*/ 0x48,  0xc7, 0xc0, 0,    0, 0, 0,    /*mov <qwords>,%rax*/
    /*e:*/ 0x48,  0x83, 0xe8, 0x01,             /*sub $0x1,%rax*/
    /*12:*/ 0x4d, 0x8b, 0x14, 0xc4,             /*mov (%r12,%rax,8),%r10*/
    /*16:*/ 0x4c, 0x89, 0x94, 0xc4, 0, 0, 0, 0, /*mov %r10,<offset>(%rsp,%rax,8)*/
    /*1e:*/ 0x48, 0x85, 0xc0,                   /*test %rax,%rax*/
    /*21:*/ 0x7f, 0xeb,                         /*jg e <L0>*/
  };
  uint8_t *addr = push_insns (insn_varr, blk_mov_pat, sizeof (blk_mov_pat));
  memcpy (addr + 3, &addr_offset, sizeof (uint32_t));
  memcpy (addr + 10, &qwords, sizeof (uint32_t));
  memcpy (addr + 26, &offset, sizeof (uint32_t));
}

static void gen_movxmm (VARR (uint8_t) * insn_varr, uint32_t offset, uint32_t reg, int b32_p,
                        int ld_p) {
  static const uint8_t ld_xmm_reg_pat[] = {
    0xf2, 0x0f, 0x10, 0x83, 0, 0, 0, 0 /* movs[sd] <offset>(%rbx),%xmm */
  };
  static const uint8_t st_xmm_reg_pat[] = {
    0xf2, 0x0f, 0x11, 0x83, 0, 0, 0, 0 /* movs[sd] %xmm, <offset>(%rbx) */
  };
  uint8_t *addr = push_insns (insn_varr, ld_p ? ld_xmm_reg_pat : st_xmm_reg_pat,
                              ld_p ? sizeof (ld_xmm_reg_pat) : sizeof (st_xmm_reg_pat));
  memcpy (addr + 4, &offset, sizeof (uint32_t));
  assert (reg <= 7);
  addr[3] |= reg << 3;
  if (b32_p) addr[0] |= 1;
}

static void gen_movxmm2 (VARR (uint8_t) * insn_varr, uint32_t offset, uint32_t reg, int ld_p) {
  static const uint8_t ld_xmm_reg_pat[] = {
    0xf2, 0x41, 0x0f, 0x10, 0x44, 0x24, 0 /* movsd <offset>(%r12),%xmm */
  };
  static const uint8_t st_xmm_reg_pat[] = {
    0xf2, 0x41, 0x0f, 0x11, 0x44, 0x24, 0 /* movsd %xmm, <offset>(%r12) */
  };
  uint8_t *addr = push_insns (insn_varr, ld_p ? ld_xmm_reg_pat : st_xmm_reg_pat,
                              ld_p ? sizeof (ld_xmm_reg_pat) : sizeof (st_xmm_reg_pat));
  addr[6] = offset;
  assert (reg <= 7);
  addr[4] |= reg << 3;
}

#ifdef _WIN32
static void gen_add (VARR (uint8_t) * insn_varr, uint32_t sp_offset, int reg) {
  static const uint8_t lea_pat[] = {
    0x48, 0x8d, 0x84, 0x24, 0, 0, 0, 0, /* lea    <sp_offset>(%sp),reg */
  };
  uint8_t *addr = push_insns (insn_varr, lea_pat, sizeof (lea_pat));
  memcpy (addr + 4, &sp_offset, sizeof (uint32_t));
  addr[2] |= (reg & 7) << 3;
  if (reg > 7) addr[0] |= 4;
}
#endif

static void gen_st (VARR (uint8_t) * insn_varr, uint32_t sp_offset, int b64_p) {
  static const uint8_t st_pat[] = {
    0x44, 0x89, 0x94, 0x24, 0, 0, 0, 0, /* mov    %r10,<sp_offset>(%sp) */
  };
  uint8_t *addr = push_insns (insn_varr, st_pat, sizeof (st_pat));
  memcpy (addr + 4, &sp_offset, sizeof (uint32_t));
  if (b64_p) addr[0] |= 8;
}

static void gen_ldst (VARR (uint8_t) * insn_varr, uint32_t sp_offset, uint32_t src_offset,
                      int b64_p) {
  static const uint8_t ld_pat[] = {
    0x44, 0x8b, 0x93, 0, 0, 0, 0, /* mov    <src_offset>(%rbx),%r10 */
  };
  uint8_t *addr = push_insns (insn_varr, ld_pat, sizeof (ld_pat));
  memcpy (addr + 3, &src_offset, sizeof (uint32_t));
  if (b64_p) addr[0] |= 8;
  gen_st (insn_varr, sp_offset, b64_p);
}

static void gen_ldst80 (VARR (uint8_t) * insn_varr, uint32_t sp_offset, uint32_t src_offset) {
  static uint8_t const ldst80_pat[] = {
    0xdb, 0xab, 0,    0, 0, 0,    /* fldt   <src_offset>(%rbx) */
    0xdb, 0xbc, 0x24, 0, 0, 0, 0, /* fstpt  <sp_offset>(%sp) */
  };
  uint8_t *addr = push_insns (insn_varr, ldst80_pat, sizeof (ldst80_pat));
  memcpy (addr + 2, &src_offset, sizeof (uint32_t));
  memcpy (addr + 9, &sp_offset, sizeof (uint32_t));
}

static void gen_st80 (VARR (uint8_t) * insn_varr, uint32_t src_offset) {
  static const uint8_t st80_pat[] = {0xdb, 0xbb, 0, 0, 0, 0 /* fstpt   <src_offset>(%rbx) */};
  memcpy (push_insns (insn_varr, st80_pat, sizeof (st80_pat)) + 2, &src_offset, sizeof (uint32_t));
}

/* Generation: fun (fun_addr, res_arg_addresses):
   push r12, push rbx; sp-=sp_offset; r11=fun_addr; rbx=res/arg_addrs
   r10=mem[rbx,<offset>]; (arg_reg=mem[r10] or r10=mem[r10];mem[sp,sp_offset]=r10
                           or r12=mem[rbx,arg_offset]; arg_reg=mem[r12]
                                                       [;(arg_reg + 1)=mem[r12 + 8]]
                           ...
                           or r12=mem[rbx,arg_offset];rax=qwords;
                              L:rax-=1;r10=mem[r12,rax]; mem[sp,sp_offset,rax]=r10;
                                goto L if rax > 0) ...
   rax=8; call *r11; sp+=offset
   r10=mem[rbx,<offset>]; res_reg=mem[r10]; ...
   pop rbx; pop r12; ret. */
void *_MIR_get_ff_call (MIR_context_t ctx, size_t nres, MIR_type_t *res_types, size_t nargs,
                        _MIR_arg_desc_t *arg_descs, size_t arg_vars_num MIR_UNUSED) {
  static const uint8_t prolog[] = {
#ifndef _WIN32
    0x41, 0x54,                   /* pushq %r12 */
    0x53,                         /* pushq %rbx */
    0x48, 0x81, 0xec, 0, 0, 0, 0, /* subq <sp_offset>, %rsp */
    0x49, 0x89, 0xfb,             /* mov $rdi, $r11 -- fun addr */
    0x48, 0x89, 0xf3,             /* mov $rsi, $rbx -- result/arg addresses */
#else
    /* 0x0: */ 0x41,  0x54,                   /* pushq %r12 */
    /* 0x2: */ 0x53,                          /* pushq %rbx */
    /* 0x3: */ 0x55,                          /* push %rbp */
    /* 0x4: */ 0x48,  0x89, 0xe5,             /* mov %rsp,%rbp */
    /* 0x7: */ 0x48,  0x81, 0xec, 0, 0, 0, 0, /* subq <sp_offset>, %rsp */
    /* 0xe: */ 0x49,  0x89, 0xcb,             /* mov $rcx, $r11 -- fun addr */
    /* 0x11: */ 0x48, 0x89, 0xd3,             /* mov $rdx, $rbx -- result/arg addresses */
#endif
  };
  static const uint8_t call_end[] = {
#ifndef _WIN32
    0x48, 0xc7, 0xc0, 0x08, 0, 0, 0, /* mov $8, rax -- to save xmm varargs */
#endif
    0x41, 0xff, 0xd3, /* callq  *%r11	   */
#ifndef _WIN32
    0x48, 0x81, 0xc4, 0,    0, 0, 0, /* addq <sp_offset>, %rsp */
#endif
  };
  static const uint8_t epilog[] = {
#ifdef _WIN32              /* Strict form of windows epilogue for unwinding: */
    0x48, 0x8d, 0x65, 0x0, /* lea  0x0(%rbp),%rsp */
    0x5d,                  /* pop %rbp */
#endif
    0x5b,       /* pop %rbx */
    0x41, 0x5c, /* pop %r12 */
    0xc3,       /* ret */
  };
#ifndef _WIN32
  static const uint8_t iregs[] = {7, 6, 2, 1, 8, 9}; /* rdi, rsi, rdx, rcx, r8, r9 */
  static const uint32_t max_iregs = 6, max_xregs = 8;
  uint32_t sp_offset = 0;
#else
  static const uint8_t iregs[] = {1, 2, 8, 9}; /* rcx, rdx, r8, r9 */
  static const uint32_t max_iregs = 4, max_xregs = 4;
  uint32_t blk_offset = nargs < 4 ? 32 : (uint32_t) nargs * 8, sp_offset = 32; /* spill area */
#endif
  uint32_t n_iregs = 0, n_xregs = 0, n_fregs, qwords;
  uint8_t *addr;
  VARR (uint8_t) * code;
  void *res;

  VARR_CREATE (uint8_t, code, ctx->alloc, 128);
  push_insns (code, prolog, sizeof (prolog));
  for (size_t i = 0; i < nargs; i++) {
    MIR_type_t type = arg_descs[i].type;

    if ((MIR_T_I8 <= type && type <= MIR_T_U64) || type == MIR_T_P || type == MIR_T_RBLK) {
      if (n_iregs < max_iregs) {
        gen_mov (code, (uint32_t) ((i + nres) * sizeof (long double)), iregs[n_iregs++], TRUE);
#ifdef _WIN32
        n_xregs++;
#endif
      } else {
        gen_ldst (code, sp_offset, (uint32_t) ((i + nres) * sizeof (long double)), TRUE);
        sp_offset += 8;
      }
    } else if (type == MIR_T_F || type == MIR_T_D) {
      if (n_xregs < max_xregs) {
        gen_movxmm (code, (uint32_t) ((i + nres) * sizeof (long double)), n_xregs++,
                    type == MIR_T_F, TRUE);
#ifdef _WIN32
        gen_mov (code, (uint32_t) ((i + nres) * sizeof (long double)), iregs[n_iregs++], TRUE);
#endif
      } else {
        gen_ldst (code, sp_offset, (uint32_t) ((i + nres) * sizeof (long double)), type == MIR_T_D);
        sp_offset += 8;
      }
    } else if (type == MIR_T_LD) {
      gen_ldst80 (code, sp_offset, (uint32_t) ((i + nres) * sizeof (long double)));
      sp_offset += 16;
    } else if (MIR_blk_type_p (type)) {
      qwords = (uint32_t) ((arg_descs[i].size + 7) / 8);
#ifndef _WIN32
      if (type == MIR_T_BLK + 1 && n_iregs + qwords <= max_iregs) {
        assert (qwords <= 2);
        gen_mov (code, (i + nres) * sizeof (long double), 12, TRUE);   /* r12 = block addr */
        gen_mov2 (code, 0, iregs[n_iregs], TRUE);                      /* arg_reg = mem[r12] */
        if (qwords == 2) gen_mov2 (code, 8, iregs[n_iregs + 1], TRUE); /* arg_reg = mem[r12 + 8] */
        n_iregs += qwords;
        n_xregs += qwords;
        continue;
      } else if (type == MIR_T_BLK + 2 && n_xregs + qwords <= max_xregs) {
        assert (qwords <= 2);
        gen_mov (code, (i + nres) * sizeof (long double), 12, TRUE); /* r12 = block addr */
        gen_movxmm2 (code, 0, n_xregs, TRUE);                        /* xmm = mem[r12] */
        if (qwords == 2) gen_movxmm2 (code, 8, n_xregs + 1, TRUE);   /* xmm = mem[r12 +  8] */
        n_xregs += qwords;
        continue;
      } else if (type == MIR_T_BLK + 3 && n_iregs < max_iregs && n_xregs < max_xregs) {
        assert (qwords == 2);
        gen_mov (code, (i + nres) * sizeof (long double), 12, TRUE); /* r12 = block addr */
        gen_mov2 (code, 0, iregs[n_iregs], TRUE);                    /* arg_reg = mem[r12] */
        n_iregs++;
        n_xregs++;
        gen_movxmm2 (code, 8, n_xregs, TRUE); /* xmm = mem[r12 + 8] */
        n_xregs++;
        continue;
      } else if (type == MIR_T_BLK + 4 && n_iregs < max_iregs && n_xregs < max_xregs) {
        assert (qwords == 2);
        gen_mov (code, (i + nres) * sizeof (long double), 12, TRUE); /* r12 = block addr */
        gen_movxmm2 (code, 0, n_xregs, TRUE);                        /* xmm = mem[r12] */
        n_xregs++;
        gen_mov2 (code, 8, iregs[n_iregs], TRUE); /* arg_reg = mem[r12 + 8] */
        n_iregs++;
        n_xregs++;
        continue;
      }
      gen_blk_mov (code, sp_offset, (i + nres) * sizeof (long double), qwords);
      sp_offset += qwords * 8;
#else
      if (qwords <= 1) {
        gen_mov (code, (uint32_t) ((i + nres) * sizeof (long double)), 12,
                 TRUE); /* r12 = mem[disp + rbx] */
        if (n_iregs < max_iregs) {
          gen_mov2 (code, 0, iregs[n_iregs++], TRUE); /* arg_reg = mem[r12] */
          n_xregs++;
        } else {
          gen_mov2 (code, 0, 10, TRUE);   /* r10 = mem[r12] */
          gen_st (code, sp_offset, TRUE); /* mem[sp+sp_offset] = r10; */
          sp_offset += 8;
        }
      } else {
        /* r12 = mem[disp + rbx]; mem[rsp+blk_offset + nw] = r10 = mem[r12 + nw]; */
        gen_blk_mov (code, blk_offset, (uint32_t) ((i + nres) * sizeof (long double)), qwords);
        if (n_iregs < max_iregs) {
          gen_add (code, blk_offset, iregs[n_iregs++]); /* arg_reg = sp + blk_offset */
          n_xregs++;
        } else {
          gen_add (code, blk_offset, 10); /* r10 = sp + blk_offset */
          gen_st (code, sp_offset, TRUE); /* mem[sp+sp_offset] = r10; */
          sp_offset += 8;
        }
        blk_offset += qwords * 8;
      }
#endif
    } else {
      MIR_get_error_func (ctx) (MIR_call_op_error, "wrong type of arg value");
    }
  }
#ifdef _WIN32
  if (blk_offset > sp_offset) sp_offset = blk_offset;
#endif
  sp_offset = (sp_offset + 15) / 16 * 16;
#ifndef _WIN32
  sp_offset += 8; /* align */
#endif
  addr = VARR_ADDR (uint8_t, code);
#ifndef _WIN32
  memcpy (addr + 6, &sp_offset, sizeof (uint32_t));
#else
  memcpy (addr + 10, &sp_offset, sizeof (uint32_t));
#endif
  addr = push_insns (code, call_end, sizeof (call_end));
#ifndef _WIN32
  memcpy (addr + sizeof (call_end) - 4, &sp_offset, sizeof (uint32_t));
#else
  if (nres > 1)
    MIR_get_error_func (ctx) (MIR_call_op_error,
                              "Windows x86-64 doesn't support multiple return values");
#endif
  n_iregs = n_xregs = n_fregs = 0;
  for (size_t i = 0; i < nres; i++) {
    if (((MIR_T_I8 <= res_types[i] && res_types[i] <= MIR_T_U64) || res_types[i] == MIR_T_P)
        && n_iregs < 2) {
      gen_mov (code, (uint32_t) (i * sizeof (long double)), n_iregs++ == 0 ? 0 : 2,
               FALSE); /* rax or rdx */
    } else if ((res_types[i] == MIR_T_F || res_types[i] == MIR_T_D) && n_xregs < 2) {
      gen_movxmm (code, (uint32_t) (i * sizeof (long double)), n_xregs++, res_types[i] == MIR_T_F,
                  FALSE);
    } else if (res_types[i] == MIR_T_LD && n_fregs < 2) {
      gen_st80 (code, (uint32_t) (i * sizeof (long double)));
    } else {
      MIR_get_error_func (ctx) (MIR_ret_error,
                                "x86-64 can not handle this combination of return values");
    }
  }
  push_insns (code, epilog, sizeof (epilog));
  res = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
  VARR_DESTROY (uint8_t, code);
  return res;
}

/* Transform C call to call of void handler (MIR_context_t ctx, MIR_item_t func_item,
                                             va_list va, MIR_val_t *results) */
void *_MIR_get_interp_shim (MIR_context_t ctx, MIR_item_t func_item, void *handler) {
  static const uint8_t push_rbx[] = {0x53, /*push   %rbx  */};
  static const uint8_t prepare_pat[] = {
#ifndef _WIN32
    /*  0: */ 0x48, 0x83, 0xec, 0x20,                      /* sub    32,%rsp	     */
    /*  4: */ 0x48, 0x89, 0xe2,                            /* mov    %rsp,%rdx	     */
    /*  7: */ 0xc7, 0x02, 0,    0,    0,    0,             /* movl   0,(%rdx)	     */
    /*  d: */ 0xc7, 0x42, 0x04, 0x30, 0,    0, 0,          /* movl   48, 4(%rdx)     */
    /* 14: */ 0x48, 0x8d, 0x44, 0x24, 0x20,                /* lea    32(%rsp),%rax   */
    /* 19: */ 0x48, 0x89, 0x42, 0x10,                      /* mov    %rax,16(%rdx)   */
    /* 1d: */ 0x48, 0x8d, 0x84, 0x24, 0xe0, 0, 0, 0,       /* lea    224(%rsp),%rax  */
    /* 25: */ 0x48, 0x89, 0x42, 0x08,                      /* mov    %rax,8(%rdx)    */
    /* 29: */ 0x48, 0x81, 0xec, 0,    0,    0, 0,          /* sub    <n>,%rsp	     */
    /* 30: */ 0x48, 0x89, 0xe3,                            /* mov    %rsp,%rbx	     */
    /* 33: */ 0x48, 0x89, 0xe1,                            /* mov    %rsp,%rcx	     */
    /* 36: */ 0x48, 0xbf, 0,    0,    0,    0, 0, 0, 0, 0, /* movabs <ctx>,%rdi      */
    /* 40: */ 0x48, 0xbe, 0,    0,    0,    0, 0, 0, 0, 0, /* movabs <func_item>,%rsi*/
    /* 4a: */ 0x48, 0xb8, 0,    0,    0,    0, 0, 0, 0, 0, /* movabs <handler>,%rax  */
    /* 54: */ 0xff, 0xd0,                                  /* callq  *%rax           */
  };
  static const uint32_t nres_offset = 0x2c;
  static const uint32_t ctx_offset = 0x38;
  static const uint32_t func_offset = 0x42;
  static const uint32_t hndl_offset = 0x4c;
  static const uint32_t prep_stack_size = 208;
#else
    /*  0: */ 0x53,                                        /* push   %rbx            */
    /*  1: */ 0x55,                                        /* push %rbp */
    /*  2: */ 0x48, 0x89, 0xe5,                            /* mov %rsp,%rbp */
    /*  5: */ 0x4c, 0x8d, 0x44, 0x24, 0x18,                /* lea    24(%rsp),%r8     */
    /*  a: */ 0x48, 0x81, 0xec, 0,    0,    0, 0,          /* sub    <n>,%rsp        */
    /* 11: */ 0x48, 0x89, 0xe3,                            /* mov    %rsp,%rbx       */
    /* 14: */ 0x49, 0x89, 0xe1,                            /* mov    %rsp,%r9        */
    /* 17: */ 0x48, 0x83, 0xec, 0x20,                      /* sub    32,%rsp         */
    /* 1b: */ 0x48, 0xb9, 0,    0,    0,    0, 0, 0, 0, 0, /* movabs <ctx>,%rcx      */
    /* 25: */ 0x48, 0xba, 0,    0,    0,    0, 0, 0, 0, 0, /* movabs <func_item>,%rdx*/
    /* 2f: */ 0x48, 0xb8, 0,    0,    0,    0, 0, 0, 0, 0, /* movabs <handler>,%rax  */
    /* 39: */ 0xff, 0xd0,                                  /* callq  *%rax           */
  };
  static const uint32_t nres_offset = 0x0d;
  static const uint32_t ctx_offset = 0x1d;
  static const uint32_t func_offset = 0x27;
  static const uint32_t hndl_offset = 0x31;
  static const uint32_t prep_stack_size = 32;
#endif
  static const uint8_t shim_end[] = {
#ifndef _WIN32
    /* 0: */ 0x48, 0x81, 0xc4, 0, 0, 0, 0, /*add    prep_stack_size+n,%rsp*/
#else                                      /* Strict form of windows epilogue for unwinding: */
    /* 0 */ 0x48,  0x8d, 0x65, 0x0, /* lea  0x0(%rbp),%rsp */
    /* 4: */ 0x5d,                  /* pop %rbp */
#endif
    0x5b, /*pop                      %rbx*/
    0xc3, /*retq                         */
  };
  static const uint8_t ld_pat[] = {0x48, 0x8b, 0x83, 0, 0, 0, 0}; /* mov <offset>(%rbx), %reg */
  static const uint8_t movss_pat[]
    = {0xf3, 0x0f, 0x10, 0x83, 0, 0, 0, 0}; /* movss <offset>(%rbx), %xmm[01] */
  static const uint8_t movsd_pat[]
    = {0xf2, 0x0f, 0x10, 0x83, 0, 0, 0, 0};                   /* movsd <offset>(%rbx), %xmm[01] */
  static const uint8_t fldt_pat[] = {0xdb, 0xab, 0, 0, 0, 0}; /* fldt <offset>(%rbx) */
  static const uint8_t fxch_pat[] = {0xd9, 0xc9};             /* fxch */
  uint8_t *addr;
  uint32_t imm, n_iregs, n_xregs, n_fregs, offset;
  uint32_t nres = func_item->u.func->nres;
  MIR_type_t *results = func_item->u.func->res_types;
  VARR (uint8_t) * code;
  void *res;

  VARR_CREATE (uint8_t, code, ctx->alloc, 128);
#ifndef _WIN32
  push_insns (code, push_rbx, sizeof (push_rbx));
#endif
  push_insns (code, save_pat, sizeof (save_pat));
  addr = push_insns (code, prepare_pat, sizeof (prepare_pat));
  imm = nres * 16;
#ifdef _WIN32
  imm += 8; /*align */
#endif
  memcpy (addr + nres_offset, &imm, sizeof (uint32_t));
  memcpy (addr + ctx_offset, &ctx, sizeof (void *));
  memcpy (addr + func_offset, &func_item, sizeof (void *));
  memcpy (addr + hndl_offset, &handler, sizeof (void *));
  /* move results: */
#ifdef _WIN32
  if (nres > 1)
    MIR_get_error_func (ctx) (MIR_call_op_error,
                              "Windows x86-64 doesn't support multiple return values");
#endif
  n_iregs = n_xregs = n_fregs = offset = 0;
  for (uint32_t i = 0; i < nres; i++) {
    if (results[i] == MIR_T_F && n_xregs < 2) {
      addr = push_insns (code, movss_pat, sizeof (movss_pat));
      addr[3] |= n_xregs << 3;
      memcpy (addr + 4, &offset, sizeof (uint32_t));
      n_xregs++;
    } else if (results[i] == MIR_T_D && n_xregs < 2) {
      addr = push_insns (code, movsd_pat, sizeof (movsd_pat));
      addr[3] |= n_xregs << 3;
      memcpy (addr + 4, &offset, sizeof (uint32_t));
      n_xregs++;
    } else if (results[i] == MIR_T_LD && n_fregs < 2) {
      addr = push_insns (code, fldt_pat, sizeof (fldt_pat));
      memcpy (addr + 2, &offset, sizeof (uint32_t));
      if (n_fregs == 1) push_insns (code, fxch_pat, sizeof (fxch_pat));
      n_fregs++;
    } else if (n_iregs < 2) {
      addr = push_insns (code, ld_pat, sizeof (ld_pat));
      addr[2] |= n_iregs << 4;
      memcpy (addr + 3, &offset, sizeof (uint32_t));
      n_iregs++;
    } else {
      MIR_get_error_func (ctx) (MIR_ret_error,
                                "x86-64 can not handle this combination of return values");
    }
    offset += 16;
  }
  addr = push_insns (code, shim_end, sizeof (shim_end));
#ifndef _WIN32
  imm = prep_stack_size + nres * 16;
  memcpy (addr + 3, &imm, sizeof (uint32_t));
#endif
  res = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
  VARR_DESTROY (uint8_t, code);
  return res;
}

/* push rsi,rdi;rsi=called_func,rdi=ctx;r10=hook_address;jmp wrapper_end; */
void *_MIR_get_wrapper (MIR_context_t ctx, MIR_item_t called_func, void *hook_address) {
#ifndef _WIN32
  static const uint8_t start_pat[] = {
    0x56,                               /* push   %rsi			   */
    0x57,                               /* push   %rdi			   */
    0x48, 0xbe, 0, 0, 0, 0, 0, 0, 0, 0, /* movabs called_func,%rsi  	   */
    0x48, 0xbf, 0, 0, 0, 0, 0, 0, 0, 0, /* movabs ctx,%rdi  	   */
    0x49, 0xba, 0, 0, 0, 0, 0, 0, 0, 0, /* movabs <hook_address>,%r10  	   */
    0xe9, 0,    0, 0, 0,                /* 0x0: jmp rel32 */
  };
  size_t call_func_offset = 4, ctx_offset = 14, hook_offset = 24, rel32_offset = 33;
#else
  static const uint8_t start_pat[] = {
    0x48, 0x89, 0x4c, 0x24, 0x08,                /* mov  %rcx,0x08(%rsp) */
    0x48, 0x89, 0x54, 0x24, 0x10,                /* mov  %rdx,0x10(%rsp) */
    0x48, 0xba, 0,    0,    0,    0, 0, 0, 0, 0, /* movabs called_func,%rdx   */
    0x48, 0xb9, 0,    0,    0,    0, 0, 0, 0, 0, /* movabs ctx,%rcx           */
    0x49, 0xba, 0,    0,    0,    0, 0, 0, 0, 0, /* movabs <hook_address>,%r10*/
    0xe9, 0,    0,    0,    0,                   /* 0x0: jmp rel32 */
  };
  size_t call_func_offset = 2, ctx_offset = 12, hook_offset = 22, rel32_offset = 31;
#endif
  uint8_t *addr;
  VARR (uint8_t) * code;
  void *res;

  VARR_CREATE (uint8_t, code, ctx->alloc, 128);
  addr = push_insns (code, start_pat, sizeof (start_pat));
  memcpy (addr + call_func_offset, &called_func, sizeof (void *));
  memcpy (addr + ctx_offset, &ctx, sizeof (void *));
  memcpy (addr + hook_offset, &hook_address, sizeof (void *));
  res = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
  VARR_DESTROY (uint8_t, code);
  int64_t off = (uint8_t *) wrapper_end_addr - ((uint8_t *) res + rel32_offset + 4);
  assert (INT32_MIN <= off && off <= INT32_MAX);
  _MIR_change_code (ctx, (uint8_t *) res + rel32_offset, (uint8_t *) &off, 4); /* LE */
  return res;
}

void *_MIR_get_wrapper_end (MIR_context_t ctx) {
#ifndef _WIN32
  static const uint8_t wrap_end[] = {
    0x50,                               /*push   %rax */
    0x53,                               /*push   %rbx */
    0x48, 0x89, 0xe0,                   /*mov    %rsp,%rax */
    0x48, 0x89, 0xc3,                   /*mov    %rax,%rbx */
    0x48, 0x83, 0xe0, 0x0f,             /*and    $0xf,%rax */
    0x48, 0x05, 0x80, 0,    0,    0,    /*add    $0x80,%rax */
    0x48, 0x29, 0xc4,                   /*sub    %rax,%rsp -- aligned now */
    0xf3, 0x0f, 0x7f, 0x04, 0x24,       /*movdqu %xmm0,(%rsp)		   */
    0xf3, 0x0f, 0x7f, 0x4c, 0x24, 0x10, /*movdqu %xmm1,0x10(%rsp)	   */
    0xf3, 0x0f, 0x7f, 0x54, 0x24, 0x20, /*movdqu %xmm2,0x20(%rsp)	   */
    0xf3, 0x0f, 0x7f, 0x5c, 0x24, 0x30, /*movdqu %xmm3,0x30(%rsp)	   */
    0xf3, 0x0f, 0x7f, 0x64, 0x24, 0x40, /*movdqu %xmm4,0x40(%rsp)	   */
    0xf3, 0x0f, 0x7f, 0x6c, 0x24, 0x50, /*movdqu %xmm5,0x50(%rsp)	   */
    0xf3, 0x0f, 0x7f, 0x74, 0x24, 0x60, /*movdqu %xmm6,0x60(%rsp)	   */
    0xf3, 0x0f, 0x7f, 0x7c, 0x24, 0x70, /*movdqu %xmm7,0x70(%rsp)	   */
    0x41, 0x51,                         /*push   %r9			   */
    0x41, 0x50,                         /*push   %r8			   */
    0x51,                               /*push   %rcx			   */
    0x52,                               /*push   %rdx			   */
    0x41, 0xff, 0xd2,                   /*callq  *%r10			   */
    0x49, 0x89, 0xc2,                   /*mov %rax,%r10		   */
    0x5a,                               /*pop    %rdx			   */
    0x59,                               /*pop    %rcx			   */
    0x41, 0x58,                         /*pop    %r8			   */
    0x41, 0x59,                         /*pop    %r9			   */
    0xf3, 0x0f, 0x6f, 0x04, 0x24,       /*movdqu (%rsp),%xmm0		   */
    0xf3, 0x0f, 0x6f, 0x4c, 0x24, 0x10, /*movdqu 0x10(%rsp),%xmm1	   */
    0xf3, 0x0f, 0x6f, 0x54, 0x24, 0x20, /*movdqu 0x20(%rsp),%xmm2	   */
    0xf3, 0x0f, 0x6f, 0x5c, 0x24, 0x30, /*movdqu 0x30(%rsp),%xmm3	   */
    0xf3, 0x0f, 0x6f, 0x64, 0x24, 0x40, /*movdqu 0x40(%rsp),%xmm4	   */
    0xf3, 0x0f, 0x6f, 0x6c, 0x24, 0x50, /*movdqu 0x50(%rsp),%xmm5	   */
    0xf3, 0x0f, 0x6f, 0x74, 0x24, 0x60, /*movdqu 0x60(%rsp),%xmm6	   */
    0xf3, 0x0f, 0x6f, 0x7c, 0x24, 0x70, /*movdqu 0x70(%rsp),%xmm7	   */
    0x48, 0x89, 0xdc,                   /*mov    %rbx,%rsp */
    0x5b,                               /*pop    %rbx */
    0x58,                               /*pop    %rax */
    0x5f,                               /*pop    %rdi			   */
    0x5e,                               /*pop    %rsi			   */
    0x41, 0xff, 0xe2,                   /*jmpq   *%r10			   */
  };
#else
  static const uint8_t wrap_end[] = {
    0x4c, 0x89, 0x44, 0x24, 0x18,       /*mov  %r8, 0x18(%rsp) */
    0x4c, 0x89, 0x4c, 0x24, 0x20,       /*mov  %r9, 0x20(%rsp) */
    0x50,                               /*push %rax               */
    0x55,                               /*push %rbp */
    0x48, 0x89, 0xe5,                   /*mov %rsp,%rbp */
    0x48, 0x89, 0xe0,                   /*mov    %rsp,%rax */
    0x48, 0x83, 0xe0, 0x0f,             /*and    $0xf,%rax */
    0x48, 0x05, 0x28, 0,    0,    0,    /*add    $0x40,%rax */
    0x48, 0x29, 0xc4,                   /*sub    %rax,%rsp -- aligned now */
    0x66, 0x0f, 0xd6, 0x04, 0x24,       /*movq   %xmm0,(%rsp) */
    0x66, 0x0f, 0xd6, 0x4c, 0x24, 0x08, /*movq   %xmm1,0x8(%rsp) */
    0x66, 0x0f, 0xd6, 0x54, 0x24, 0x10, /*movq   %xmm2,0x10(%rsp) */
    0x66, 0x0f, 0xd6, 0x5c, 0x24, 0x18, /*movq   %xmm3,0x18(%rsp) */
    0x41, 0xff, 0xd2,                   /*callq  *%r10              */
    0x49, 0x89, 0xc2,                   /*mov    %rax,%r10          */
    0xf3, 0x0f, 0x7e, 0x04, 0x24,       /*movq (%rsp),%xmm0*/
    0xf3, 0x0f, 0x7e, 0x4c, 0x24, 0x08, /*movq 0x8(%rsp),%xmm1*/
    0xf3, 0x0f, 0x7e, 0x54, 0x24, 0x10, /*movq 0x10(%rsp),%xmm2*/
    0xf3, 0x0f, 0x7e, 0x5c, 0x24, 0x18, /*movq 0x18(%rsp),%xmm3*/
    0x48, 0x89, 0xec,                   /*mov    %rbp,%rsp */
    0x5d,                               /*pop    %rbp */
    0x58,                               /*pop    %rax               */
    0x48, 0x8b, 0x4c, 0x24, 0x08,       /*mov  0x08(%rsp),%rcx */
    0x48, 0x8b, 0x54, 0x24, 0x10,       /*mov  0x10(%rsp),%rdx */
    0x4c, 0x8b, 0x44, 0x24, 0x18,       /*mov  0x18(%rsp),%r8  */
    0x4c, 0x8b, 0x4c, 0x24, 0x20,       /*mov  0x20(%rsp),%r9  */
    0x41, 0xff, 0xe2,                   /*jmpq   *%r10			   */
  };
#endif
  VARR (uint8_t) * code;
  void *res;

  VARR_CREATE (uint8_t, code, ctx->alloc, 128);
  push_insns (code, wrap_end, sizeof (wrap_end));
  res = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
  VARR_DESTROY (uint8_t, code);
  return res;
}

/* r10=<bb_version>; jump rex32  ??? mutex free */
void *_MIR_get_bb_thunk (MIR_context_t ctx, void *bb_version, void *handler) {
  void *res;
  int32_t disp;
  static const uint8_t pattern[] = {
    0x49, 0xba, 0, 0, 0, 0, 0, 0, 0, 0, /* 0x0: movabsq 0, r10 */
    0xe9, 0,    0, 0, 0,                /* 0xa: jmpq <rel32> */
  };
  res = _MIR_publish_code (ctx, pattern, sizeof (pattern));
  _MIR_update_code (ctx, res, 1, 2, bb_version);
  disp = (int32_t) ((char *) handler - ((char *) res + sizeof (pattern)));
  _MIR_change_code (ctx, (uint8_t *) res + 11, (uint8_t *) &disp, 4);
  return res;
}

/* change to jmp rex32(to) */
void _MIR_replace_bb_thunk (MIR_context_t ctx, void *thunk, void *to) {
  uint8_t op = 0xe9; /* jmpq */
  int32_t disp;
  _MIR_change_code (ctx, (uint8_t *) thunk, &op, 1); /* jmpq <disp32> */
  disp = (int32_t) ((char *) to - ((char *) thunk + 5));
  _MIR_change_code (ctx, (uint8_t *) thunk + 1, (uint8_t *) &disp, 4);
}

static const uint8_t save_pat2[] = {
#ifndef _WIN32
  0x48, 0x81, 0xec, 0x80, 0,    0,    0, /*sub    $0x80,%rsp		   */
  0xf3, 0x0f, 0x7f, 0x04, 0x24,          /*movdqu %xmm0,(%rsp)		   */
  0xf3, 0x0f, 0x7f, 0x4c, 0x24, 0x10,    /*movdqu %xmm1,0x10(%rsp)	   */
  0xf3, 0x0f, 0x7f, 0x54, 0x24, 0x20,    /*movdqu %xmm2,0x20(%rsp)	   */
  0xf3, 0x0f, 0x7f, 0x5c, 0x24, 0x30,    /*movdqu %xmm3,0x30(%rsp)	   */
  0xf3, 0x0f, 0x7f, 0x64, 0x24, 0x40,    /*movdqu %xmm4,0x40(%rsp)	   */
  0xf3, 0x0f, 0x7f, 0x6c, 0x24, 0x50,    /*movdqu %xmm5,0x50(%rsp)	   */
  0xf3, 0x0f, 0x7f, 0x74, 0x24, 0x60,    /*movdqu %xmm6,0x60(%rsp)	   */
  0xf3, 0x0f, 0x7f, 0x7c, 0x24, 0x70,    /*movdqu %xmm7,0x70(%rsp)	   */
  0x41, 0x51,                            /*push   %r9			   */
  0x41, 0x50,                            /*push   %r8			   */
  0x51,                                  /*push   %rcx			   */
  0x52,                                  /*push   %rdx			   */
  0x56,                                  /*push   %rsi			   */
  0x57,                                  /*push   %rdi			   */
#else
  0x48, 0x89, 0x4c, 0x24, 0x08,          /*mov  %rcx,0x08(%rsp) */
  0x48, 0x89, 0x54, 0x24, 0x10,          /*mov  %rdx,0x10(%rsp) */
  0x4c, 0x89, 0x44, 0x24, 0x18,          /*mov  %r8, 0x18(%rsp) */
  0x4c, 0x89, 0x4c, 0x24, 0x20,          /*mov  %r9, 0x20(%rsp) */
  0x48, 0x81, 0xec, 0x80, 0,    0,    0, /*sub    $0x60,%rsp		   */
  0xf3, 0x0f, 0x7f, 0x04, 0x24,          /*movdqu %xmm0,(%rsp)		   */
  0xf3, 0x0f, 0x7f, 0x4c, 0x24, 0x10,    /*movdqu %xmm1,0x10(%rsp)	   */
  0xf3, 0x0f, 0x7f, 0x54, 0x24, 0x20,    /*movdqu %xmm2,0x20(%rsp)	   */
  0xf3, 0x0f, 0x7f, 0x5c, 0x24, 0x30,    /*movdqu %xmm3,0x30(%rsp)	   */
  0xf3, 0x0f, 0x7f, 0x64, 0x24, 0x40,    /*movdqu %xmm4,0x40(%rsp)	   */
  0xf3, 0x0f, 0x7f, 0x6c, 0x24, 0x50,    /*movdqu %xmm5,0x50(%rsp)	   */
#endif
  0x50,       /*push   %rax			   */
  0x41, 0x53, /*push   %r11			   */
};

static const uint8_t restore_pat2[] = {
  0x41, 0x5b, /*pop    %r11			   */
  0x58,       /*pop    %rax			   */
#ifndef _WIN32
  0x5f,                                  /*pop    %rdi			   */
  0x5e,                                  /*pop    %rsi			   */
  0x5a,                                  /*pop    %rdx			   */
  0x59,                                  /*pop    %rcx			   */
  0x41, 0x58,                            /*pop    %r8			   */
  0x41, 0x59,                            /*pop    %r9			   */
  0xf3, 0x0f, 0x6f, 0x04, 0x24,          /*movdqu (%rsp),%xmm0		   */
  0xf3, 0x0f, 0x6f, 0x4c, 0x24, 0x10,    /*movdqu 0x10(%rsp),%xmm1	   */
  0xf3, 0x0f, 0x6f, 0x54, 0x24, 0x20,    /*movdqu 0x20(%rsp),%xmm2	   */
  0xf3, 0x0f, 0x6f, 0x5c, 0x24, 0x30,    /*movdqu 0x30(%rsp),%xmm3	   */
  0xf3, 0x0f, 0x6f, 0x64, 0x24, 0x40,    /*movdqu 0x40(%rsp),%xmm4	   */
  0xf3, 0x0f, 0x6f, 0x6c, 0x24, 0x50,    /*movdqu 0x50(%rsp),%xmm5	   */
  0xf3, 0x0f, 0x6f, 0x74, 0x24, 0x60,    /*movdqu 0x60(%rsp),%xmm6	   */
  0xf3, 0x0f, 0x6f, 0x7c, 0x24, 0x70,    /*movdqu 0x70(%rsp),%xmm7	   */
  0x48, 0x81, 0xc4, 0x80, 0,    0,    0, /*add    $0x80,%rsp		   */
#else
  0xf3, 0x0f, 0x6f, 0x04, 0x24,          /*movdqu (%rsp),%xmm0		   */
  0xf3, 0x0f, 0x6f, 0x4c, 0x24, 0x10,    /*movdqu 0x10(%rsp),%xmm1	   */
  0xf3, 0x0f, 0x6f, 0x54, 0x24, 0x20,    /*movdqu 0x20(%rsp),%xmm2	   */
  0xf3, 0x0f, 0x6f, 0x5c, 0x24, 0x30,    /*movdqu 0x30(%rsp),%xmm3	   */
  0xf3, 0x0f, 0x6f, 0x64, 0x24, 0x40,    /*movdqu 0x40(%rsp),%xmm4	   */
  0xf3, 0x0f, 0x6f, 0x6c, 0x24, 0x50,    /*movdqu 0x50(%rsp),%xmm5	   */
  0x48, 0x81, 0xc4, 0x80, 0,    0,    0, /*add    $0x60,%rsp		   */
  0x48, 0x8b, 0x4c, 0x24, 0x08,          /*mov  0x08(%rsp),%rcx */
  0x48, 0x8b, 0x54, 0x24, 0x10,          /*mov  0x10(%rsp),%rdx */
  0x4c, 0x8b, 0x44, 0x24, 0x18,          /*mov  0x18(%rsp),%r8  */
  0x4c, 0x8b, 0x4c, 0x24, 0x20,          /*mov  0x20(%rsp),%r9  */
#endif
};

/* save all clobbered regs but 10; r10 = call hook_address (data, r10); restore regs; jmp *r10
   r10 is a generator temp reg which is not used across bb borders. */
void *_MIR_get_bb_wrapper (MIR_context_t ctx, void *data, void *hook_address) {
  static const uint8_t wrap_end[] = {
    0x41, 0xff, 0xe2, /*jmpq   *%r10			   */
  };
  static const uint8_t call_pat[] =
#ifndef _WIN32
    {
      0x4c, 0x89, 0xd6,                         /* mov %r10,%rsi */
      0x48, 0xbf, 0,    0,    0, 0, 0, 0, 0, 0, /* movabs data,%rdi */
      0x49, 0xba, 0,    0,    0, 0, 0, 0, 0, 0, /* movabs <hook_address>,%r10 */
      0x48, 0x89, 0xe2,                         /* mov    %rsp,%rdx */
      0x48, 0x83, 0xe2, 0x0f,                   /* and    $0xf,%rdx */
      0x74, 0x07,                               /* je     10 <l> */
      0x52,                                     /* push   %rdx */
      0x41, 0xff, 0xd2,                         /* callq  *%r10 */
      0x5a,                                     /* pop    %rdx */
      0xeb, 0x03,                               /* jmp    13 <l2> */
      0x41, 0xff, 0xd2,                         /* l: callq  *%r10 */
      0x49, 0x89, 0xc2,                         /* l2:mov %rax,%r10 */
    };
  size_t data_offset = 5, hook_offset = 15;
#else
    {
      0x55,                                     /* push %rbp */
      0x48, 0x89, 0xe5,                         /* mov %rsp,%rbp */
      0x4c, 0x89, 0xd2,                         /* mov %r10,%rdx   */
      0x48, 0xb9, 0,    0,    0, 0, 0, 0, 0, 0, /* movabs data,%rcx           */
      0x49, 0xba, 0,    0,    0, 0, 0, 0, 0, 0, /* movabs <hook_address>,%r10*/
      0x50,                                     /* push   %rax               */
      0x48, 0x83, 0xec, 0x28,                   /* sub    40,%rsp            */
      0x41, 0xff, 0xd2,       /* callq  *%r10       ???align for unaligned sp       */
      0x49, 0x89, 0xc2,       /* mov    %rax,%r10          */
      0x48, 0x83, 0xc4, 0x28, /* add    40,%rsp            */
      0x58,                   /* pop    %rax               */
      0x5d,                   /* pop %rbp */
    };
  size_t data_offset = 9, hook_offset = 19;
#endif
  uint8_t *addr;
  VARR (uint8_t) * code;
  void *res;

  VARR_CREATE (uint8_t, code, ctx->alloc, 128);
  push_insns (code, save_pat2, sizeof (save_pat2));
  addr = push_insns (code, call_pat, sizeof (call_pat));
  memcpy (addr + data_offset, &data, sizeof (void *));
  memcpy (addr + hook_offset, &hook_address, sizeof (void *));
  push_insns (code, restore_pat2, sizeof (restore_pat2));
  push_insns (code, wrap_end, sizeof (wrap_end));
  res = _MIR_publish_code (ctx, VARR_ADDR (uint8_t, code), VARR_LENGTH (uint8_t, code));
  VARR_DESTROY (uint8_t, code);
  return res;
}
