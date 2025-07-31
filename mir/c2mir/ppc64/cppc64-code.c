/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include "../mirc.h"
#include "mirc_ppc64_linux.h"

#include "mirc_ppc64_float.h"
#include "mirc_ppc64_limits.h"
#include "mirc_ppc64_stdarg.h"
#include "mirc_ppc64_stdint.h"
#include "mirc_ppc64_stddef.h"

static string_include_t standard_includes[]
  = {{NULL, mirc}, {NULL, ppc64_mirc}, TARGET_STD_INCLUDES};

#define MAX_ALIGNMENT 16

#define ADJUST_VAR_ALIGNMENT(c2m_ctx, align, type) ppc64_adjust_var_alignment (c2m_ctx, align, type)

static int ppc64_adjust_var_alignment (c2m_ctx_t c2m_ctx MIR_UNUSED, int align MIR_UNUSED,
                                       struct type *type MIR_UNUSED) {
  return align;
}

static int invalid_alignment (mir_llong align) {
  return align != 0 && align != 1 && align != 2 && align != 4 && align != 8 && align != 16;
}
