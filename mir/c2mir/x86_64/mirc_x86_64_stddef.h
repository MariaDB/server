/* This file is a part of MIR project.
   Copyright (C) 2019-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* See C11 7.19 */
static char stddef_str[]
  = "#ifndef __STDDEF_H\n"
    "#define __STDDEF_H\n"
    "\n"
#ifdef _WIN32
    "typedef long long int ptrdiff_t;\n"
    "typedef unsigned long long size_t;\n"
#else
    "typedef long ptrdiff_t;\n"
    "typedef unsigned long size_t;\n"
#endif
    "typedef long double max_align_t;\n"
#if defined(_WIN32)
    "typedef unsigned short wchar_t;\n"
#else
    "typedef int wchar_t;\n"
#endif
    "\n"
#if !defined(__APPLE__) && !defined(_WIN32)
    "#define NULL ((void *) 0)\n"
#endif
    "\n"
    "#define offsetof(type, member_designator) ((size_t) & ((type *) 0)->member_designator)\n"
    "\n"
    "#endif /* #ifndef __STDDEF_H */\n";
