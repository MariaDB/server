/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* See 5.2.4.2 */
static char limits_str[]
  = "#ifndef __LIMITS_H\n"
    "#define __LIMITS_H\n"
    "\n"
    "#define CHAR_BIT 8\n"
    "\n"
    "#define SCHAR_MIN (-SCHAR_MAX - 1)\n"
    "#define SCHAR_MAX 127\n"
    "#define UCHAR_MAX (SCHAR_MAX * 2 + 1)\n"
    "\n"
    "#define MB_LEN_MAX 1\n"
    "\n"
    "#define SHRT_MIN (-SHRT_MAX - 1)\n"
    "#define SHRT_MAX 32767\n"
    "#define USHRT_MAX (SHRT_MAX * 2 + 1)\n"
    "\n"
    "#define INT_MIN (-INT_MAX - 1)\n"
    "#define INT_MAX 2147483647\n"
    "#define UINT_MAX (INT_MAX * 2u + 1u)\n"
    "\n"
    "#define LONG_MIN (-LONG_MAX - 1l)\n"
    "#define LONG_MAX 9223372036854775807l\n"
    "#define ULONG_MAX (LONG_MAX * 2ul + 1ul)\n"
    "\n"
    "#define LLONG_MIN LONG_MIN\n"
    "#define LLONG_MAX LONG_MAX\n"
    "#define ULLONG_MAX ULONG_MAX\n"
    "\n"
    "/* signed char by default */\n"
    "#define CHAR_MIN SCHAR_MIN\n"
    "#define CHAR_MAX SCHAR_MAX\n"
    "\n"
    "#endif /* #ifndef __LIMITS_H */\n";
