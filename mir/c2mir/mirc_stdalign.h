/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* See C11 7.15 */
static char stdalign_str[]
  = "#ifndef __STDALIGN_H\n"
    "#define __STDALIGN_H\n"
    "\n"
    "#define alignas _Alignas\n"
    "#define alignof _Alignof\n"
    "#define __alignas_is_defined 1\n"
    "#define __alignof_is_defined 1\n"
    "#endif /* #ifndef __STDALIGN_H */\n";
