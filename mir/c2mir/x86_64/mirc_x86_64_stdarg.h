/* This file is a part of MIR project.
   Copyright (C) 2019-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* See C11 7.16 and https://gitlab.com/x86-psABIs/x86-64-ABI */
static char stdarg_str[]
  = "#ifndef __STDARG_H\n"
    "#define __STDARG_H\n"
    "\n"
#if defined(__APPLE__)
    "typedef __darwin_va_list va_list;\n"
#elif defined(__WIN32)
    "typedef char *va_list;\n"
#elif defined(__GNU_LIBRARY__)
    "typedef struct {\n"
    "  unsigned int gp_offset;\n"
    "  unsigned int fp_offset;\n"
    "  void *overflow_arg_area;\n"
    "  void *reg_save_area;\n"
    "} va_list[1];\n"
#endif
    "\n"
#if defined(__WIN32)
    "#define va_start(ap, param) __va_start (ap, param)\n"
#else
    "#define va_start(ap, param) __builtin_va_start (ap)\n"
#endif
    "#define va_arg(ap, type) __builtin_va_arg(ap, (type *) 0)\n"
    "#define va_end(ap) 0\n"
#if defined(__APPLE__) || defined(__WIN32)
    "#define va_copy(dest, src) ((dest) = (src))\n"
#else
    "#define va_copy(dest, src) ((dest)[0] = (src)[0])\n"
#endif
    "\n"
    "/* For standard headers of a GNU system: */\n"
    "#ifndef __GNUC_VA_LIST\n"
    "#define __GNUC_VA_LIST 1\n"
    "#endif\n"
    "typedef va_list __gnuc_va_list;\n"
    "#endif /* #ifndef __STDARG_H */\n";
