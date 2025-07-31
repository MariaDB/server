/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* See C11 7.18 */
static char stdbool_str[]
  = "#ifndef __STDBOOL_H\n"
    "#define __STDBOOL_H\n"
    "\n"
    "#define bool _Bool\n"
    "#define true 1\n"
    "#define false 0\n"
    "#define __bool_true_false_are_defined 1\n"
    "#endif /* #ifndef __STDBOOL_H */\n";
