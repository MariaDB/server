/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

static const char mirc[]
  = "#define __mirc__ 1\n"
    "#define __MIRC__ 1\n"
    "#define __STDC_HOSTED__ 1\n"
    "//#define __STDC_ISO_10646__ 201103L\n"
    "#define __STDC_NO_ATOMICS__ 1\n"
    "#define __STDC_NO_COMPLEX__ 1\n"
    "#define __STDC_NO_THREADS__ 1\n"
    "#define __STDC_NO_VLA__ 1\n"
    "#define __STDC_UTF_16__ 1\n"
    "#define __STDC_UTF_32__ 1\n"
    "#define __STDC_VERSION__ 201112L\n"
    "#define __STDC__ 1\n"
    "\n"
    "/* Some GCC alternative keywords used but not defined in standard headers:  */\n"
    "#define __const const\n"
    "#define __const__ const\n"
    "#define __inline__ inline\n"
    "#define __restrict__ restrict\n"
    "#define __signed signed\n"
    "#define __signed__ signed\n"
    "#define __volatile volatile\n"
    "#define __volatile__ volatile\n";

#include "mirc_iso646.h"
#include "mirc_stdalign.h"
#include "mirc_stdbool.h"
#include "mirc_stdnoreturn.h"

#define TARGET_STD_INCLUDES                                                               \
  {"iso646.h", iso646_str}, {"stdalign.h", stdalign_str}, {"stdbool.h", stdbool_str},     \
    {"stdnoreturn.h", stdnoreturn_str}, {"float.h", float_str}, {"limits.h", limits_str}, \
    {"stdarg.h", stdarg_str}, {"stdint.h", stdint_str}, {                                 \
    "stddef.h", stddef_str                                                                \
  }
