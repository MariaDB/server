/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#ifndef C2MIR_H

#define C2MIR_H

#include "mir.h"

#define COMMAND_LINE_SOURCE_NAME "<command-line>"
#define STDIN_SOURCE_NAME "<stdin>"

struct c2mir_macro_command {
  int def_p;              /* #define or #undef */
  const char *name, *def; /* def is used only when def_p is true */
};

struct c2mir_options {
  FILE *message_file;
  int debug_p, verbose_p, ignore_warnings_p, no_prepro_p, prepro_only_p;
  int syntax_only_p, pedantic_p, asm_p, object_p;
  size_t module_num;
  FILE *prepro_output_file; /* non-null for prepro_only_p */
  const char *output_file_name;
  size_t macro_commands_num, include_dirs_num;
  struct c2mir_macro_command *macro_commands;
  const char **include_dirs;
};

void c2mir_init (MIR_context_t ctx);
void c2mir_finish (MIR_context_t ctx);
int c2mir_compile (MIR_context_t ctx, struct c2mir_options *ops, int (*getc_func) (void *),
                   void *getc_data, const char *source_name, FILE *output_file);

#endif
