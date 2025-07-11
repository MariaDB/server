#include "mir.h"
#include "mir-gen.h"
#include <string.h>

int main(int argc, char **argv)
{
  int interp_p = 0, gen_p = 0;
  switch (argc) {
  case 1:
    break;
  case 2:
    interp_p = !strcmp(argv[1], "-i");
    if (interp_p)
      break;
    gen_p = !strcmp(argv[1], "-g");
    if (gen_p)
      break;
    fprintf (stderr, "%s: unknown option %s\n", argv[0], argv[1]);
  default:
    fprintf (stderr, "%s: [-i|-g]\n", argv[0]);
    return 1;
  }

  MIR_context_t ctx = MIR_init();
  MIR_module_t mir_module = MIR_new_module(ctx, "hello");
  MIR_item_t gv = MIR_new_data(ctx, "greetings", MIR_T_U8, 12,
                               "world\0all\0\0");
  MIR_type_t i = MIR_T_I32;
  MIR_item_t callback = MIR_new_proto(ctx, "cb", 1, &i, 1, MIR_T_P, "string");
  MIR_item_t func = MIR_new_func(ctx, "hello", 1, &i, 3,
                                 MIR_T_P, "string", MIR_T_P, "callback",
                                 MIR_T_I32, "id");
  MIR_reg_t temp = MIR_new_func_reg(ctx, func->u.func, MIR_T_I64, "$temp");
  MIR_reg_t ret = MIR_new_func_reg(ctx, func->u.func, MIR_T_I64, "$ret");
  MIR_reg_t string = MIR_reg(ctx, "string", func->u.func),
    cb = MIR_reg(ctx, "callback", func->u.func),
    id = MIR_reg(ctx, "id", func->u.func);
  MIR_append_insn(ctx, func,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, temp),
                               MIR_new_ref_op(ctx, gv)));
  MIR_append_insn(ctx, func,
                  MIR_new_insn(ctx, MIR_MUL, MIR_new_reg_op(ctx, id),
                               MIR_new_reg_op(ctx, id),
                               MIR_new_int_op(ctx, 6)));
  MIR_append_insn(ctx, func,
                  MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, id),
                               MIR_new_reg_op(ctx, id),
                               MIR_new_reg_op(ctx, temp)));
  MIR_append_insn(ctx, func,
                  MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, callback),
                                    MIR_new_reg_op(ctx, cb),
                                    MIR_new_reg_op(ctx, ret),
                                    MIR_new_reg_op(ctx, string)));
  MIR_append_insn(ctx, func,
                  MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, callback),
                                    MIR_new_reg_op(ctx, cb),
                                    MIR_new_reg_op(ctx, temp),
                                    MIR_new_reg_op(ctx, id)));
  MIR_append_insn(ctx, func,
                  MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, ret),
                               MIR_new_reg_op(ctx, ret),
                               MIR_new_reg_op(ctx, temp)));
  MIR_append_insn(ctx, func,
                  MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, ret)));
  MIR_finish_func(ctx);
  MIR_finish_module(ctx);
  // MIR_output(ctx, stderr);
  MIR_load_module(ctx, mir_module);
  // MIR_output(ctx, stderr);
  MIR_gen_init(ctx, 1);
  typedef int (*Callback)(const char*);
  int (*boo) (const char *, Callback, unsigned) = NULL;
  if (interp_p) {
    MIR_link(ctx, MIR_set_interp_interface, NULL);
    boo = func->addr;
  } else if (gen_p) {
    MIR_link(ctx, MIR_set_gen_interface, NULL);
    boo = MIR_gen(ctx, 0, func);
  } else {
    MIR_output(ctx, stderr);
    goto cleanup;
  }
  printf("%s: %d\n", interp_p ? "interpreted" : "compiled",
         boo("hello", puts, 0) + boo("goodbye", puts, 1));
cleanup:
  MIR_gen_finish(ctx);
  MIR_finish(ctx);
  return 0;
}
