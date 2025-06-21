#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alloca.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include "mir-alloc-default.c"
#include "mir-gen.h"  // mir.h gets included as well

#define MIR_TYPE_INTERP 1
#define MIR_TYPE_INTERP_NAME "interp"
#define MIR_TYPE_GEN 2
#define MIR_TYPE_GEN_NAME "gen"
#define MIR_TYPE_LAZY 3
#define MIR_TYPE_LAZY_NAME "lazy"

#define MIR_TYPE_DEFAULT MIR_TYPE_LAZY

#define MIR_ENV_VAR_LIB_DIRS "MIR_LIB_DIRS"
#define MIR_ENV_VAR_EXTRA_LIBS "MIR_LIBS"
#define MIR_ENV_VAR_TYPE "MIR_TYPE"

struct lib {
  char *name;
  void *handler;
};
typedef struct lib lib_t;

/* stdlibs according to c2mir */
#if defined(__unix__)
#if UINTPTR_MAX == 0xffffffff
static lib_t std_libs[]
  = {{"/lib/libc.so.6", NULL},   {"/lib32/libc.so.6", NULL},     {"/lib/libm.so.6", NULL},
     {"/lib32/libm.so.6", NULL}, {"/lib/libpthread.so.0", NULL}, {"/lib32/libpthread.so.0", NULL}};
static const char *std_lib_dirs[] = {"/lib", "/lib32"};
#elif UINTPTR_MAX == 0xffffffffffffffff
#if defined(__x86_64__)
static lib_t std_libs[] = {{"/lib64/libc.so.6", NULL},
                           {"/lib/x86_64-linux-gnu/libc.so.6", NULL},
                           {"/lib64/libm.so.6", NULL},
                           {"/lib/x86_64-linux-gnu/libm.so.6", NULL},
                           {"/usr/lib64/libpthread.so.0", NULL},
                           {"/lib/x86_64-linux-gnu/libpthread.so.0", NULL},
                           {"/usr/lib/libc.so", NULL}};
static const char *std_lib_dirs[] = {"/lib64", "/lib/x86_64-linux-gnu"};
#elif (__aarch64__)
static lib_t std_libs[]
  = {{"/lib64/libc.so.6", NULL},       {"/lib/aarch64-linux-gnu/libc.so.6", NULL},
     {"/lib64/libm.so.6", NULL},       {"/lib/aarch64-linux-gnu/libm.so.6", NULL},
     {"/lib64/libpthread.so.0", NULL}, {"/lib/aarch64-linux-gnu/libpthread.so.0", NULL}};
static const char *std_lib_dirs[] = {"/lib64", "/lib/aarch64-linux-gnu"};
#elif (__PPC64__)
static lib_t std_libs[] = {
  {"/lib64/libc.so.6", NULL},
  {"/lib64/libm.so.6", NULL},
  {"/lib64/libpthread.so.0", NULL},
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  {"/lib/powerpc64le-linux-gnu/libc.so.6", NULL},
  {"/lib/powerpc64le-linux-gnu/libm.so.6", NULL},
  {"/lib/powerpc64le-linux-gnu/libpthread.so.0", NULL},
#else
  {"/lib/powerpc64-linux-gnu/libc.so.6", NULL},
  {"/lib/powerpc64-linux-gnu/libm.so.6", NULL},
  {"/lib/powerpc64-linux-gnu/libpthread.so.0", NULL},
#endif
};
static const char *std_lib_dirs[] = {
  "/lib64",
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  "/lib/powerpc64le-linux-gnu",
#else
  "/lib/powerpc64-linux-gnu",
#endif
};
#elif (__s390x__)
static lib_t std_libs[]
  = {{"/lib64/libc.so.6", NULL},       {"/lib/s390x-linux-gnu/libc.so.6", NULL},
     {"/lib64/libm.so.6", NULL},       {"/lib/s390x-linux-gnu/libm.so.6", NULL},
     {"/lib64/libpthread.so.0", NULL}, {"/lib/s390x-linux-gnu/libpthread.so.0", NULL}};
static const char *std_lib_dirs[] = {"/lib64", "/lib/s390x-linux-gnu"};
#elif (__riscv)
static lib_t std_libs[]
  = {{"/lib64/libc.so.6", NULL},       {"/lib/riscv64-linux-gnu/libc.so.6", NULL},
     {"/lib64/libm.so.6", NULL},       {"/lib/riscv64-linux-gnu/libm.so.6", NULL},
     {"/lib64/libpthread.so.0", NULL}, {"/lib/riscv64-linux-gnu/libpthread.so.0", NULL}};
static const char *std_lib_dirs[] = {"/lib64", "/lib/riscv64-linux-gnu"};
#else
#error cannot recognize 32- or 64-bit target
#endif
#endif
static const char *lib_suffix = ".so";
#endif

#ifdef _WIN32
static const int slash = '\\';
#else
static const int slash = '/';
#endif

#if defined(__APPLE__)
static lib_t std_libs[] = {{"/usr/lib/libc.dylib", NULL}, {"/usr/lib/libm.dylib", NULL}};
static const char *std_lib_dirs[] = {"/usr/lib"};
static const char *lib_suffix = ".dylib";
#endif

#ifdef _WIN32
static lib_t std_libs[] = {{"C:\\Windows\\System32\\msvcrt.dll", NULL},
                           {"C:\\Windows\\System32\\kernel32.dll", NULL},
                           {"C:\\Windows\\System32\\ucrtbase.dll", NULL}};
static const char *std_lib_dirs[] = {"C:\\Windows\\System32"};
static const char *lib_suffix = ".dll";
#define dlopen(n, f) LoadLibrary (n)
#define dlclose(h) FreeLibrary (h)
#define dlsym(h, s) GetProcAddress (h, s)
#endif

static void close_std_libs (void) {
  for (int i = 0; i < sizeof (std_libs) / sizeof (lib_t); i++)
    if (std_libs[i].handler != NULL) dlclose (std_libs[i].handler);
}

static void open_std_libs (void) {
  for (int i = 0; i < sizeof (std_libs) / sizeof (struct lib); i++)
    std_libs[i].handler = dlopen (std_libs[i].name, RTLD_LAZY);
}

DEF_VARR (lib_t);
static VARR (lib_t) * extra_libs;

typedef const char *char_ptr_t;
DEF_VARR (char_ptr_t);
static VARR (char_ptr_t) * lib_dirs;

DEF_VARR (char);
static VARR (char) * temp_string;

static void *open_lib (const char *dir, const char *name) {
  const char *last_slash = strrchr (dir, slash);
  void *res;
  FILE *f;

  VARR_TRUNC (char, temp_string, 0);
  VARR_PUSH_ARR (char, temp_string, dir, strlen (dir));
  if (last_slash == NULL || last_slash[1] != '\0') VARR_PUSH (char, temp_string, slash);
#ifndef _WIN32
  VARR_PUSH_ARR (char, temp_string, "lib", 3);
#endif
  VARR_PUSH_ARR (char, temp_string, name, strlen (name));
  VARR_PUSH_ARR (char, temp_string, lib_suffix, strlen (lib_suffix));
  VARR_PUSH (char, temp_string, 0);
  if ((res = dlopen (VARR_ADDR (char, temp_string), RTLD_LAZY)) == NULL) {
#ifndef _WIN32
    if ((f = fopen (VARR_ADDR (char, temp_string), "rb")) != NULL) {
      fclose (f);
      fprintf (stderr, "loading %s:%s\n", VARR_ADDR (char, temp_string), dlerror ());
    }
#endif
  }
  return res;
}

static void process_extra_lib (char *lib_name) {
  lib_t lib;

  lib.name = lib_name;
  for (size_t i = 0; i < VARR_LENGTH (char_ptr_t, lib_dirs); i++)
    if ((lib.handler = open_lib (VARR_GET (char_ptr_t, lib_dirs, i), lib_name)) != NULL) break;
  if (lib.handler == NULL) {
    fprintf (stderr, "cannot find library lib%s -- good bye\n", lib_name);
    exit (1);
  }
  VARR_PUSH (lib_t, extra_libs, lib);
}

static void close_extra_libs (void) {
  void *handler;

  for (size_t i = 0; i < VARR_LENGTH (lib_t, extra_libs); i++)
    if ((handler = VARR_GET (lib_t, extra_libs, i).handler) != NULL) dlclose (handler);
}

#if defined(__APPLE__) && defined(__aarch64__)
float __nan (void) {
  union {
    uint32_t i;
    float f;
  } u = {0x7fc00000};
  return u.f;
}
#endif

static void *import_resolver (const char *name) {
  void *handler, *sym = NULL;

  for (int i = 0; i < sizeof (std_libs) / sizeof (struct lib); i++)
    if ((handler = std_libs[i].handler) != NULL && (sym = dlsym (handler, name)) != NULL) break;
  if (sym == NULL)
    for (int i = 0; i < VARR_LENGTH (lib_t, extra_libs); i++)
      if ((handler = VARR_GET (lib_t, extra_libs, i).handler) != NULL
          && (sym = dlsym (handler, name)) != NULL)
        break;
  if (sym == NULL) {
#ifdef _WIN32
    if (strcmp (name, "LoadLibrary") == 0) return LoadLibrary;
    if (strcmp (name, "FreeLibrary") == 0) return FreeLibrary;
    if (strcmp (name, "GetProcAddress") == 0) return GetProcAddress;
#else
    if (strcmp (name, "dlopen") == 0) return dlopen;
    if (strcmp (name, "dlerror") == 0) return dlerror;
    if (strcmp (name, "dlclose") == 0) return dlclose;
    if (strcmp (name, "dlsym") == 0) return dlsym;
    if (strcmp (name, "stat") == 0) return stat;
    if (strcmp (name, "lstat") == 0) return lstat;
    if (strcmp (name, "fstat") == 0) return fstat;
#if defined(__APPLE__) && defined(__aarch64__)
    if (strcmp (name, "__nan") == 0) return __nan;
    if (strcmp (name, "_MIR_set_code") == 0) return _MIR_set_code;
#endif
#endif
    fprintf (stderr, "can not load symbol %s\n", name);
    close_std_libs ();
    exit (1);
  }
  return sym;
}

void lib_dirs_from_env_var (const char *env_var) {
  const char *var_value = getenv (env_var);
  if (var_value == NULL || var_value[0] == '\0') return;

  // copy to an allocated buffer
  int value_len = strlen (var_value);
  char *value = (char *) malloc (value_len + 1);
  strcpy (value, var_value);

  // colon separated list
  char *value_ptr = value;
  char *colon = NULL;
  while ((colon = strchr (value_ptr, ':')) != NULL) {
    colon[0] = '\0';
    VARR_PUSH (char_ptr_t, lib_dirs, value_ptr);
    // goto next
    value_ptr = colon + 1;
  }
  // final part of string
  // colon == NULL
  VARR_PUSH (char_ptr_t, lib_dirs, value_ptr);
}

int get_mir_type (void) {
  const char *type_value = getenv (MIR_ENV_VAR_TYPE);
  if (type_value == NULL || type_value[0] == '\0') return MIR_TYPE_DEFAULT;

  if (strcmp (type_value, MIR_TYPE_INTERP_NAME) == 0) return MIR_TYPE_INTERP;

  if (strcmp (type_value, MIR_TYPE_GEN_NAME) == 0) return MIR_TYPE_GEN;

  if (strcmp (type_value, MIR_TYPE_LAZY_NAME) == 0) return MIR_TYPE_LAZY;

  fprintf (stderr, "warning: unknown MIR_TYPE '%s', using default one\n", type_value);
  return MIR_TYPE_DEFAULT;
}

void open_extra_libs (void) {
  const char *var_value = getenv (MIR_ENV_VAR_EXTRA_LIBS);
  if (var_value == NULL || var_value[0] == '\0') return;

  int value_len = strlen (var_value);
  char *value = (char *) malloc (value_len + 1);
  strcpy (value, var_value);

  char *value_ptr = value;
  char *colon = NULL;
  while ((colon = strchr (value_ptr, ':')) != NULL) {
    colon[0] = '\0';
    process_extra_lib (value_ptr);

    value_ptr = colon + 1;
  }
  process_extra_lib (value_ptr);
}

int main (int argc, char **argv, char **envp) {
  MIR_alloc_t alloc = &default_alloc;
  // from binfmt_misc we expect the arguments to be:
  // `mir-run /full/path/to/mir-binary mir-binary <args...>`
  if (argc < 3) {
    fprintf (stderr, "usage: %s <full-path> <name> [<args>...]\n", argv[0]);
    return 1;
  }

  int mir_type = get_mir_type ();

  MIR_val_t val;
  int exit_code;

  VARR_CREATE (char, temp_string, alloc, 0);
  VARR_CREATE (lib_t, extra_libs, alloc, 16);
  VARR_CREATE (char_ptr_t, lib_dirs, alloc, 16);
  for (int i = 0; i < sizeof (std_lib_dirs) / sizeof (char_ptr_t); i++)
    VARR_PUSH (char_ptr_t, lib_dirs, std_lib_dirs[i]);
  lib_dirs_from_env_var ("LD_LIBRARY_PATH");
  lib_dirs_from_env_var (MIR_ENV_VAR_LIB_DIRS);

  MIR_item_t main_func = NULL;

  MIR_context_t mctx = MIR_init ();
  FILE *mir_file = fopen (argv[1], "rb");
  if (!mir_file) {
    fprintf (stderr, "failed to open file '%s'\n", argv[1]);
    return 1;
  }
  MIR_read (mctx, mir_file);

  for (MIR_module_t module = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (mctx)); module != NULL;
       module = DLIST_NEXT (MIR_module_t, module)) {
    for (MIR_item_t func = DLIST_HEAD (MIR_item_t, module->items); func != NULL;
         func = DLIST_NEXT (MIR_item_t, func)) {
      if (func->item_type != MIR_func_item) continue;
      if (strcmp (func->u.func->name, "main") == 0) main_func = func;
    }
    MIR_load_module (mctx, module);
  }
  if (main_func == NULL) {
    fprintf (stderr, "cannot execute program w/o main function\n");
    return 1;
  }

  open_std_libs ();
  open_extra_libs ();

  if (mir_type == MIR_TYPE_INTERP) {
    MIR_link (mctx, MIR_set_interp_interface, import_resolver);
    MIR_interp (mctx, main_func, &val, 3, (MIR_val_t){.i = (argc - 2)},
                (MIR_val_t){.a = (void *) (argv + 2)}, (MIR_val_t){.a = (void *) envp});
    exit_code = val.i;
  } else {
    MIR_gen_init (mctx);
    MIR_link (mctx, mir_type == MIR_TYPE_GEN ? MIR_set_gen_interface : MIR_set_lazy_gen_interface,
              import_resolver);
    uint64_t (*fun_addr) (int, char **, char **) = MIR_gen (mctx, main_func);
    exit_code = fun_addr (argc - 2, argv + 2, envp);
    MIR_gen_finish (mctx);
  }
  MIR_finish (mctx);
  close_extra_libs ();
  close_std_libs ();

  return exit_code;
}
