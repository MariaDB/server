#include <string.h>
#include "../mir.h"
#include "../mir-gen.h"
#include "test-read.h"

static char *program_str = NULL;
static void our_exit (int code) {
  if (program_str != NULL) free (program_str);
  exit (code);
}

int main (int argc, char *argv[]) {
  const char *mir_fname = NULL;
  MIR_module_t mir_module;
  MIR_item_t f, main_func;
  MIR_val_t val;
  int interpr_p, gen_p, debug_p, execute_p;
  int res;
  uint64_t (*fun_addr) (void);
  MIR_context_t ctx;

  if (argc != 2 && argc != 3) {
    fprintf (stderr, "%s: [-i|-g|-d|-gd] <input mir file>\n", argv[0]);
    exit (1);
  }
  interpr_p = gen_p = debug_p = FALSE;
  execute_p = TRUE;
  if (strcmp (argv[1], "-i") == 0) {
    interpr_p = TRUE;
  } else if (strcmp (argv[1], "-g") == 0) {
    gen_p = TRUE;
  } else if (strcmp (argv[1], "-gd") == 0) {
    gen_p = debug_p = TRUE;
  } else if (strcmp (argv[1], "-d") == 0) {
    gen_p = debug_p = TRUE;
    execute_p = FALSE;
  } else if (argc == 3) {
    fprintf (stderr, "%s: unknown option %s\n", argv[0], argv[1]);
    exit (1);
  }
  mir_fname = argv[argc - 1];
  ctx = MIR_init ();
  program_str = read_file (mir_fname);
  MIR_scan_string (ctx, program_str);
  mir_module = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx));
  if (DLIST_NEXT (MIR_module_t, mir_module) != NULL) {
    fprintf (stderr, "%s: there should be one module in the file %s\n", argv[0], mir_fname);
    our_exit (1);
  }
  if (!gen_p && !interpr_p) MIR_output (ctx, stderr);
  main_func = NULL;
  for (f = DLIST_HEAD (MIR_item_t, mir_module->items); f != NULL; f = DLIST_NEXT (MIR_item_t, f))
    if (f->item_type == MIR_func_item && strcmp (f->u.func->name, "main") == 0) main_func = f;
  if (main_func == NULL && execute_p) {
    fprintf (stderr, "%s: cannot execute program w/o main function\n", argv[0]);
    our_exit (1);
  }
  MIR_load_module (ctx, mir_module);
  if (!gen_p && !interpr_p) {
    fprintf (stderr, "+++++++++++++++++++After simplification:+++++++++++++++\n");
    MIR_output (ctx, stderr);
  }
  MIR_load_external (ctx, "abort", abort);
  MIR_load_external (ctx, "exit", our_exit);
  MIR_load_external (ctx, "printf", printf);
  MIR_load_external (ctx, "malloc", malloc);
  MIR_load_external (ctx, "free", free);
  if (interpr_p) {
    assert (execute_p);
    MIR_link (ctx, MIR_set_interp_interface, NULL);
    MIR_interp (ctx, main_func, &val, 0);
    fprintf (stderr, "%s: %lu\n", mir_fname, (unsigned long) val.i);
  } else if (gen_p) {
    MIR_gen_init (ctx);
    if (debug_p) MIR_gen_set_debug_file (ctx, stderr);
    if (execute_p) {
      MIR_link (ctx, MIR_set_gen_interface, NULL);
      fun_addr = MIR_gen (ctx, main_func);
      res = (int) fun_addr ();
      fprintf (stderr, "%s: %d\n", mir_fname, res);
    }
    MIR_gen_finish (ctx);
  } else {
    MIR_link (ctx, MIR_set_interp_interface, NULL);
    fprintf (stderr, "+++++++++++++++++++After inlining:+++++++++++++++\n");
    MIR_output (ctx, stderr);
  }
  MIR_finish (ctx);
  our_exit (0);
}
