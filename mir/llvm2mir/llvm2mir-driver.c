/* This file is a part of MIR project.
   Copyright (C) 2019-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.

   Driver to translate bitcode into MIR and execute the generated MIR.
*/

#define _GNU_SOURCE /* for mempcpy */
#include "llvm2mir.h"
#include <llvm-c/BitReader.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include "mir.h"
#include "mir-gen.h"
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>

static void fancy_abort (void) {
  fprintf (stderr, "Test failed\n");
  abort ();
}

static void llvm_memset_p0i8_i32 (uint8_t *dest, uint8_t v, uint32_t len, uint32_t align,
                                  uint8_t volatile_p) {
  memset (dest, v, len);
}
static void llvm_memset_p0i8_i64 (uint8_t *dest, uint8_t v, uint64_t len, uint32_t align,
                                  uint8_t volatile_p) {
  memset (dest, v, len);
}
static void llvm_memcpy_p0i8_p0i8_i32 (uint8_t *dest, uint8_t *src, uint32_t len, uint32_t align,
                                       uint8_t volatile_p) {
  memcpy (dest, src, len);
}
static void llvm_memcpy_p0i8_p0i8_i64 (uint8_t *dest, uint8_t *src, uint64_t len, uint32_t align,
                                       uint8_t volatile_p) {
  memcpy (dest, src, len);
}
static void llvm_memmove_p0i8_p0i8_i32 (uint8_t *dest, uint8_t *src, uint32_t len, uint32_t align,
                                        uint8_t volatile_p) {
  memmove (dest, src, len);
}
static void llvm_memmove_p0i8_p0i8_i64 (uint8_t *dest, uint8_t *src, uint64_t len, uint32_t align,
                                        uint8_t volatile_p) {
  memmove (dest, src, len);
}
static void llvm_va_copy (va_list dst, va_list src) { va_copy (dst, src); }
static float llvm_trap (float v) {
  fprintf (stderr, "llvm.trap\n");
  exit (1);
}
static float llvm_fabs_f32 (float v) { return fabsf (v); }
static double llvm_fabs_f64 (double v) { return fabs (v); }

static struct lib {
  char *name;
  void *handler;
} std_libs[] = {{"/lib64/libc.so.6", NULL}, {"/lib64/libm.so.6", NULL}};

static void close_libs (void) {
  for (int i = 0; i < sizeof (std_libs) / sizeof (struct lib); i++)
    if (std_libs[i].handler != NULL) dlclose (std_libs[i].handler);
}

static void open_libs (void) {
  for (int i = 0; i < sizeof (std_libs) / sizeof (struct lib); i++)
    std_libs[i].handler = dlopen (std_libs[i].name, RTLD_LAZY);
}

static void *import_resolver (const char *name) {
  void *sym = NULL;

  for (int i = 0; i < sizeof (std_libs) / sizeof (struct lib); i++)
    if (std_libs[i].handler != NULL && (sym = dlsym (std_libs[i].handler, name)) != NULL) break;
  if (sym == NULL) {
    fprintf (stderr, "can not load symbol %s\n", name);
    close_libs ();
    exit (1);
  }
  return sym;
}

int main (int argc, char *argv[], char *env[]) {
  const char *llvm_ir_fname = NULL;
  LLVMMemoryBufferRef memory_buffer;
  LLVMModuleRef module;
  MIR_module_t mir_module;
  char *message;
  MIR_item_t f, main_func;
  MIR_val_t val;
  int interpr_p, gen_p, gen_debug_p;
  int res;
  uint64_t (*fun_addr) (int, char *argv[], char *env[]);
  MIR_context_t context;

  if (!(2 <= argc && argc <= 4)) {
    fprintf (stderr, "%s: [-dg] [-i|-g] <input bitcode file>\n", argv[0]);
    exit (1);
  }
  interpr_p = gen_p = gen_debug_p = FALSE;
  for (int i = 1; i < argc; i++)
    if (strcmp (argv[i], "-i") == 0) {
      interpr_p = TRUE;
    } else if (strcmp (argv[i], "-g") == 0) {
      gen_p = TRUE;
    } else if (strcmp (argv[i], "-dg") == 0) {
      gen_debug_p = TRUE;
    } else if (argv[i][0] == '-') {
      fprintf (stderr, "%s: unknown option %s\n", argv[0], argv[i]);
      exit (1);
    } else {
      llvm_ir_fname = argv[i];
    }
  if ((argc != 2 && argc != 3) || llvm_ir_fname == NULL) {
    fprintf (stderr, "%s: [-i|-g] <input bitcode file>\n", argv[0]);
    exit (1);
  }
  if (LLVMCreateMemoryBufferWithContentsOfFile (llvm_ir_fname, &memory_buffer, &message)) {
    fprintf (stderr, "%s\n", message);
    free (message);
    exit (1);
  }
  if (LLVMParseBitcode2 (memory_buffer, &module)) {
    fprintf (stderr, "Invalid bitcode file %s\n", argv[0]);
    LLVMDisposeMemoryBuffer (memory_buffer);
    exit (1);
  }
  LLVMDisposeMemoryBuffer (memory_buffer);
  context = MIR_init ();
  mir_module = llvm2mir (context, module);
  LLVMDisposeModule (module);
  if (!gen_p && !interpr_p) MIR_output (context, stderr);
  main_func = NULL;
  for (f = DLIST_HEAD (MIR_item_t, mir_module->items); f != NULL; f = DLIST_NEXT (MIR_item_t, f))
    if (f->item_type == MIR_func_item && strcmp (f->u.func->name, "main") == 0) main_func = f;
  if (main_func == NULL) {
    fprintf (stderr, "%s: cannot execute program w/o main function\n", argv[0]);
    exit (1);
  }
  open_libs ();
  MIR_load_module (context, mir_module);
  if (!gen_p && !interpr_p) {
    fprintf (stderr, "++++++ Test after simplification:\n");
    MIR_output (context, stderr);
    exit (0);
  }
  MIR_load_external (context, "abort", fancy_abort);
  MIR_load_external (context, "llvm.floor.f64", floor);
  MIR_load_external (context, "llvm.memset.p0i8.i32", llvm_memset_p0i8_i32);
  MIR_load_external (context, "llvm.memset.p0i8.i64", llvm_memset_p0i8_i64);
  MIR_load_external (context, "llvm.memcpy.p0i8.p0i8.i32", llvm_memcpy_p0i8_p0i8_i32);
  MIR_load_external (context, "llvm.memcpy.p0i8.p0i8.i64", llvm_memcpy_p0i8_p0i8_i64);
  MIR_load_external (context, "llvm.memmove.p0i8.p0i8.i32", llvm_memmove_p0i8_p0i8_i32);
  MIR_load_external (context, "llvm.memmove.p0i8.p0i8.i64", llvm_memmove_p0i8_p0i8_i64);
  MIR_load_external (context, "llvm.va_copy", llvm_va_copy);
  MIR_load_external (context, "llvm.trap", llvm_trap);
  MIR_load_external (context, "llvm.fabs.f32", llvm_fabs_f32);
  MIR_load_external (context, "llvm.fabs.f64", llvm_fabs_f64);
  if (interpr_p) {
    MIR_link (context, MIR_set_interp_interface, import_resolver);
    MIR_interp (context, main_func, &val, 3, (MIR_val_t){.i = 1}, (MIR_val_t){.a = (void *) argv},
                (MIR_val_t){.a = (void *) env});
    res = val.i;
  } else if (gen_p) {
    MIR_gen_init (context, 1);
    if (gen_debug_p) MIR_gen_set_debug_file (context, 0, stderr);
    MIR_link (context, MIR_set_gen_interface, import_resolver);
    fun_addr = MIR_gen (context, 0, main_func);
    res = fun_addr (argc, argv, env);
    MIR_gen_finish (context);
  }
  fprintf (stderr, "%s: %d\n", llvm_ir_fname, res);
  MIR_finish (context);
  close_libs ();
  exit (res);
}
