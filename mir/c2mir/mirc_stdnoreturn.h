/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* See C11 7.23 */
static char stdnoreturn_str[]
  = "#ifndef __STDNORETURN_H\n"
    "#define __STDNORETURN_H\n"
    "\n"
    "#define noreturn _Noreturn\n"
    "#endif /* #ifndef __STDNORETURN_H */\n";
