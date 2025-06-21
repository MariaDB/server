/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

static char stdarg_str[]
  = "#ifndef __STDARG_H\n"
    "#define __STDARG_H\n"
    "\n"
#if defined(__APPLE__)
    "typedef __darwin_va_list va_list;\n"
#elif defined(__GNU_LIBRARY__)
    "typedef struct {\n"
    "  void *__stack;\n"
    "  void *__gr_top;\n"
    "  void *__vr_top;\n"
    "  int __gr_offs;\n"
    "  int __vr_offs;\n"
    "} va_list;\n"
#endif
    "\n"
    "#define va_start(ap, param) __builtin_va_start (ap)\n"
    "#define va_arg(ap, type) __builtin_va_arg(ap, (type *) 0)\n"
    "#define va_end(ap) 0\n"
    "#define va_copy(dest, src) ((dest)[0] = (src)[0])\n"
    "\n"
    "/* For standard headers of a GNU system: */\n"
    "#ifndef __GNUC_VA_LIST\n"
    "#define __GNUC_VA_LIST 1\n"
    "#endif\n"
    "typedef va_list __gnuc_va_list;\n"
    "#endif /* #ifndef __STDARG_H */\n";
