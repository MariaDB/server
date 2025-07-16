/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include "../mirc.h"

#ifndef _WIN32
#include "mirc_x86_64_linux.h"
#else
#include "mirc_x86_64_win.h"
#endif

#include "mirc_x86_64_float.h"
#include "mirc_x86_64_limits.h"
#include "mirc_x86_64_stdarg.h"
#include "mirc_x86_64_stdint.h"
#include "mirc_x86_64_stddef.h"

static string_include_t standard_includes[]
  = {{NULL, mirc}, {NULL, x86_64_mirc}, TARGET_STD_INCLUDES};

#define MAX_ALIGNMENT 16

#define ADJUST_VAR_ALIGNMENT(c2m_ctx, align, type) x86_adjust_var_alignment (c2m_ctx, align, type)

static int x86_adjust_var_alignment (c2m_ctx_t c2m_ctx, int align, struct type *type) {
  /* see https://gitlab.com/x86-psABIs/x86-64-ABI */
  if (type->mode == TM_ARR && raw_type_size (c2m_ctx, type) >= 16) return 16;
  return align;
}

static int invalid_alignment (mir_llong align) {
  return align != 0 && align != 1 && align != 2 && align != 4 && align != 8 && align != 16;
}
