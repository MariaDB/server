/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include "../mirc.h"
#include "mirc_s390x_linux.h"

#include "mirc_s390x_float.h"
#include "mirc_s390x_limits.h"
#include "mirc_s390x_stdarg.h"
#include "mirc_s390x_stdint.h"
#include "mirc_s390x_stddef.h"

static string_include_t standard_includes[]
  = {{NULL, mirc}, {NULL, s390x_mirc}, TARGET_STD_INCLUDES};

#define MAX_ALIGNMENT 16

#define ADJUST_VAR_ALIGNMENT(c2m_ctx, align, type) s390x_adjust_var_alignment (c2m_ctx, align, type)

static int s390x_adjust_var_alignment (c2m_ctx_t c2m_ctx MIR_UNUSED, int align,
                                       struct type *type MIR_UNUSED) {
  return align;
}

static int invalid_alignment (mir_llong align) {
  return align != 0 && align != 1 && align != 2 && align != 4 && align != 8 && align != 16;
}
