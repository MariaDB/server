/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>

#ifndef _WIN32
#include <dlfcn.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifndef C2MIR_PARALLEL
#define C2MIR_PARALLEL 0
#endif

#if C2MIR_PARALLEL && defined(_WIN32) /* TODO: Win thread primitives ??? */
#undef C2MIR_PARALLEL
#define C2MIR_PARALLEL 0
#endif

#if C2MIR_PARALLEL
#include <pthread.h>
typedef pthread_mutex_t mir_mutex_t;
typedef pthread_cond_t mir_cond_t;
typedef pthread_attr_t mir_thread_attr_t;
#define mir_thread_create(m, attr, f, arg) pthread_create (m, attr, f, arg)
#define mir_thread_join(t, r) pthread_join (t, r)
#define mir_mutex_init(m, a) pthread_mutex_init (m, a)
#define mir_mutex_destroy(m) pthread_mutex_destroy (m)
#define mir_mutex_lock(m) pthread_mutex_lock (m)
#define mir_mutex_unlock(m) pthread_mutex_unlock (m)
#define mir_cond_init(m, a) pthread_cond_init (m, a)
#define mir_cond_destroy(m) pthread_cond_destroy (m)
#define mir_cond_wait(c, m) pthread_cond_wait (c, m)
#define mir_cond_signal(c) pthread_cond_signal (c)
#define mir_cond_broadcast(c) pthread_cond_broadcast (c)
#define mir_thread_attr_init(a) pthread_attr_init (a)
#define mir_thread_attr_setstacksize(a, s) pthread_attr_setstacksize (a, s)
#else
#define mir_mutex_init(m, a) 0
#define mir_mutex_destroy(m) 0
#define mir_mutex_lock(m) 0
#define mir_mutex_unlock(m) 0
#endif

#include "c2mir.h"
#include "mir-gen.h"
#include "real-time.h"

#include "mir-alloc-default.c"

struct lib {
  char *name;
  void *handler;
};

typedef struct lib lib_t;

#if defined(__unix__)
#if UINTPTR_MAX == 0xffffffff
static lib_t std_libs[]
  = {{"/lib/libc.so", NULL},         {"/lib/libm.so", NULL},          {"/lib/libc.so.6", NULL},
     {"/lib32/libc.so.6", NULL},     {"/lib/libm.so.6", NULL},        {"/lib32/libm.so.6", NULL},
     {"/lib/libpthread.so.0", NULL}, {"/lib32/libpthread.so.0", NULL}};
static const char *std_lib_dirs[] = {"/lib", "/lib32"};
#elif UINTPTR_MAX == 0xffffffffffffffff
#if defined(__x86_64__)
static lib_t std_libs[] = {{"/lib64/libc.so", NULL},
                           {"/lib/libm.so.6", NULL},
                           {"/lib64/libc.so.6", NULL},
                           {"/lib/x86_64-linux-gnu/libc.so.6", NULL},
                           {"/lib64/libm.so.6", NULL},
                           {"/lib/x86_64-linux-gnu/libm.so.6", NULL},
                           {"/usr/lib64/libpthread.so.0", NULL},
                           {"/lib/x86_64-linux-gnu/libpthread.so.0", NULL},
                           {"/usr/lib/libc.so", NULL}};
static const char *std_lib_dirs[] = {"/lib64", "/lib/x86_64-linux-gnu"};
#elif (__aarch64__)
static lib_t std_libs[]
  = {{"/lib64/libc.so", NULL},         {"/lib64/libm.so", NULL},
     {"/lib64/libc.so.6", NULL},       {"/lib/aarch64-linux-gnu/libc.so.6", NULL},
     {"/lib64/libm.so.6", NULL},       {"/lib/aarch64-linux-gnu/libm.so.6", NULL},
     {"/lib64/libpthread.so.0", NULL}, {"/lib/aarch64-linux-gnu/libpthread.so.0", NULL}};
static const char *std_lib_dirs[] = {"/lib64", "/lib/aarch64-linux-gnu"};
#elif (__PPC64__)
static lib_t std_libs[] = {
  {"/lib64/libc.so", NULL},
  {"/lib64/libm.so", NULL},
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
  = {{"/lib64/libc.so", NULL},         {"/lib64/libm.so", NULL},
     {"/lib64/libc.so.6", NULL},       {"/lib/s390x-linux-gnu/libc.so.6", NULL},
     {"/lib64/libm.so.6", NULL},       {"/lib/s390x-linux-gnu/libm.so.6", NULL},
     {"/lib64/libpthread.so.0", NULL}, {"/lib/s390x-linux-gnu/libpthread.so.0", NULL}};
static const char *std_lib_dirs[] = {"/lib64", "/lib/s390x-linux-gnu"};
#elif (__riscv)
static lib_t std_libs[]
  = {{"/lib64/libc.so", NULL},         {"/lib64/libm.so", NULL},
     {"/lib64/libc.so.6", NULL},       {"/lib/riscv64-linux-gnu/libc.so.6", NULL},
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

static struct c2mir_options options;
static int gen_debug_level;

typedef void *void_ptr_t;

DEF_VARR (void_ptr_t);
static VARR (void_ptr_t) * allocated;

static void *reg_malloc (size_t s) {
  void *res = MIR_malloc (&default_alloc, s);

  if (res == NULL) {
    fprintf (stderr, "c2m: no memory\n");
    exit (1);
  }
  VARR_PUSH (void_ptr_t, allocated, res);
  return res;
}

DEF_VARR (char);
static VARR (char) * temp_string;

typedef const char *char_ptr_t;

DEF_VARR (char_ptr_t);
static VARR (char_ptr_t) * headers;

static int interp_exec_p, gen_exec_p, lazy_gen_exec_p, lazy_bb_gen_exec_p;
static VARR (char_ptr_t) * exec_argv;
static VARR (char_ptr_t) * source_file_names;

typedef struct c2mir_macro_command macro_command_t;

DEF_VARR (macro_command_t);
static VARR (macro_command_t) * macro_commands;

static void close_std_libs (void) {
  for (size_t i = 0; i < sizeof (std_libs) / sizeof (lib_t); i++)
    if (std_libs[i].handler != NULL) dlclose (std_libs[i].handler);
}

static void open_std_libs (void) {
  for (size_t i = 0; i < sizeof (std_libs) / sizeof (struct lib); i++)
    std_libs[i].handler = dlopen (std_libs[i].name, RTLD_LAZY);
}

DEF_VARR (lib_t);
static VARR (lib_t) * cmdline_libs;
static VARR (char_ptr_t) * lib_dirs;

static void *open_lib (const char *dir, const char *name) {
  const char *last_slash = strrchr (dir, slash);
  void *res;

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
    FILE *f;
    if ((f = fopen (VARR_ADDR (char, temp_string), "rb")) != NULL) {
      fclose (f);
      fprintf (stderr, "loading %s:%s\n", VARR_ADDR (char, temp_string), dlerror ());
    }
#endif
  }
  return res;
}

static void process_cmdline_lib (char *lib_name) {
  lib_t lib;

  lib.name = lib_name;
  for (size_t i = 0; i < VARR_LENGTH (char_ptr_t, lib_dirs); i++)
    if ((lib.handler = open_lib (VARR_GET (char_ptr_t, lib_dirs, i), lib_name)) != NULL) break;
  if (lib.handler == NULL) {
    fprintf (stderr, "cannot find library lib%s -- good bye\n", lib_name);
    exit (1);
  }
  VARR_PUSH (lib_t, cmdline_libs, lib);
}

static void close_cmdline_libs (void) {
  void *handler;

  for (size_t i = 0; i < VARR_LENGTH (lib_t, cmdline_libs); i++)
    if ((handler = VARR_GET (lib_t, cmdline_libs, i).handler) != NULL) dlclose (handler);
}

static int optimize_level, threads_num;

DEF_VARR (uint8_t);
struct input {
  const char *input_name;
  size_t curr_char, code_len;
  const uint8_t *code;
  VARR (uint8_t) * code_container; /* NULL for cmd line input */
  struct c2mir_options options;
};
typedef struct input input_t;

static struct input curr_input;
DEF_VARR (input_t);
static VARR (input_t) * inputs_to_compile;

#define STRINGIFY(v) #v
#define STRING(v) STRINGIFY (v)

static void init_options (int argc, char *argv[]) {
  int incl_p, ldir_p = FALSE; /* to remove an uninitialized warning */

  options.message_file = stderr;
  options.output_file_name = NULL;
  options.debug_p = options.verbose_p = options.ignore_warnings_p = FALSE;
  options.asm_p = options.object_p = options.no_prepro_p = options.prepro_only_p = FALSE;
  options.syntax_only_p = options.pedantic_p = FALSE;
  gen_debug_level = -1;
  VARR_CREATE (char, temp_string, &default_alloc, 0);
  VARR_CREATE (char_ptr_t, headers, &default_alloc, 0);
  VARR_CREATE (macro_command_t, macro_commands, &default_alloc, 0);
  optimize_level = -1;
  threads_num = 1;
  curr_input.code = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp (argv[i], "-d") == 0) {
      options.verbose_p = options.debug_p = TRUE;
    } else if (strncmp (argv[i], "-dg", 3) == 0) {
      gen_debug_level = argv[i][3] != '\0' ? atoi (&argv[i][3]) : INT_MAX;
    } else if (strcmp (argv[i], "-S") == 0) {
      options.asm_p = TRUE;
    } else if (strcmp (argv[i], "-c") == 0) {
      options.object_p = TRUE;
    } else if (strcmp (argv[i], "-w") == 0) {
      options.ignore_warnings_p = TRUE;
    } else if (strcmp (argv[i], "-v") == 0) {
      options.verbose_p = TRUE;
    } else if (strcmp (argv[i], "-E") == 0) {
      options.prepro_only_p = TRUE;
    } else if (strcmp (argv[i], "-fsyntax-only") == 0) {
      options.syntax_only_p = TRUE;
    } else if (strcmp (argv[i], "-fpreprocessed") == 0) {
      options.no_prepro_p = TRUE;
    } else if (strcmp (argv[i], "-pedantic") == 0) {
      options.pedantic_p = TRUE;
    } else if (strncmp (argv[i], "-O", 2) == 0) {
      optimize_level = argv[i][2] != '\0' ? atoi (&argv[i][2]) : 2;
    } else if (strcmp (argv[i], "-o") == 0) {
      if (i + 1 >= argc)
        fprintf (stderr, "-o without argument\n");
      else
        options.output_file_name = argv[++i];
    } else if ((incl_p = strncmp (argv[i], "-I", 2) == 0)
               || (ldir_p = strncmp (argv[i], "-L", 2) == 0) || strncmp (argv[i], "-l", 2) == 0) {
      char *arg;
      const char *dir = strlen (argv[i]) == 2 && i + 1 < argc ? argv[++i] : argv[i] + 2;

      if (*dir == '\0') continue;
      arg = reg_malloc (strlen (dir) + 1);
      strcpy (arg, dir);
      if (incl_p || ldir_p)
        VARR_PUSH (char_ptr_t, incl_p ? headers : lib_dirs, arg);
      else
        process_cmdline_lib (arg);
    } else if (strncmp (argv[i], "-U", 2) == 0 || strncmp (argv[i], "-D", 2) == 0) {
      char *str;
      const char *bound, *def = strlen (argv[i]) == 2 && i + 1 < argc ? argv[++i] : argv[i] + 2;
      struct c2mir_macro_command macro_command = {FALSE, NULL, NULL};

      if (argv[i][1] == 'U') {
        macro_command.name = def;
      } else {
        macro_command.def_p = TRUE;
        macro_command.name = str = reg_malloc (strlen (def) + 1);
        strcpy (str, def);
        if ((bound = strchr (def, '=')) == NULL) {
          macro_command.def = "1";
        } else {
          str[bound - def] = '\0';
          macro_command.def = &macro_command.name[bound - def + 1];
        }
      }
      VARR_PUSH (macro_command_t, macro_commands, macro_command);
    } else if (strcmp (argv[i], "-i") == 0) {
      VARR_PUSH (char_ptr_t, source_file_names, STDIN_SOURCE_NAME);
    } else if (strcmp (argv[i], "-ei") == 0 || strcmp (argv[i], "-eg") == 0
               || strcmp (argv[i], "-el") == 0 || strcmp (argv[i], "-eb") == 0) {
      VARR_TRUNC (char_ptr_t, exec_argv, 0);
      if (strcmp (argv[i], "-ei") == 0)
        interp_exec_p = TRUE;
      else if (strcmp (argv[i], "-eg") == 0)
        gen_exec_p = TRUE;
      else if (strcmp (argv[i], "-el") == 0)
        lazy_gen_exec_p = TRUE;
      else
        lazy_bb_gen_exec_p = TRUE;
      VARR_PUSH (char_ptr_t, exec_argv, "c2m");
      for (i++; i < argc; i++) VARR_PUSH (char_ptr_t, exec_argv, argv[i]);
    } else if (strcmp (argv[i], "-s") == 0 && i + 1 < argc) { /* C code from cmd line */
      curr_input.code = (uint8_t *) argv[++i];
      curr_input.code_len = strlen ((char *) curr_input.code);
    } else if (strncmp (argv[i], "-p", 2) == 0) {
      threads_num = argv[i][2] != '\0' ? atoi (&argv[i][2]) : 4;
      if (threads_num <= 0) threads_num = 1;
    } else if (*argv[i] != '-') {
      VARR_PUSH (char_ptr_t, source_file_names, argv[i]);
    } else if (strcmp (argv[i], "-h") == 0) {
      fprintf (stderr,
               "Usage: %s options (-i | -s \"program\" | source files); where options are:\n",
               argv[0]);
      fprintf (stderr, "\n");
      fprintf (stderr, "  -v, -d -- output work, parser debug info\n");
      fprintf (stderr, "  -dg[level] -- output given (or max) level MIR-generator debug info\n");
      fprintf (stderr, "  -E -- output C preprocessed code into stdout\n");
      fprintf (stderr, "  -Dname[=value], -Uname -- predefine or unpredefine macros\n");
      fprintf (stderr, "  -Idir, -Ldir -- add directories to search include headers or lbraries\n");
      fprintf (stderr, "  -fpreprocessed -- assume preprocessed input C\n");
      fprintf (stderr, "  -fsyntax-only -- check C code correctness only\n");
      fprintf (stderr, "  -fpedantic -- assume strict standard input C code\n");
      fprintf (stderr, "  -w -- do not print any warnings\n");
      fprintf (stderr, "  -S, -c -- generate corresponding textual or binary MIR files\n");
      fprintf (stderr, "  -o file -- put output code into given file\n");
      fprintf (stderr, "  -On -- use given optimization level in MIR-generator\n");
      fprintf (stderr, "  -p[n] -- use given parallelism level in C2MIR and MIR-generator\n");
      fprintf (stderr, "  -ei -- execute code in the interpreter with given options\n");
      fprintf (stderr, "         (all trailing args are passed to the program)\n");
      fprintf (stderr, "  -eg -- execute code generated with given options\n");
      fprintf (stderr, "  -el -- execute code lazily generated code with given options\n");
      fprintf (stderr, "  -eb -- execute code lazily generated BB code with given options\n");
      fprintf (stderr, "%s version commit=%s\n", argv[0], STRING (GITCOMMIT));
      exit (0);
    } else {
      fprintf (stderr, "unknown command line option %s (use -h for usage) -- goodbye\n", argv[i]);
      exit (1);
    }
  }
  options.include_dirs_num = VARR_LENGTH (char_ptr_t, headers);
  options.include_dirs = VARR_ADDR (char_ptr_t, headers);
  options.macro_commands_num = VARR_LENGTH (macro_command_t, macro_commands);
  options.macro_commands = VARR_ADDR (macro_command_t, macro_commands);
  if (!C2MIR_PARALLEL || threads_num <= 1) threads_num = 0;
}

static int t_getc (void *data) {
  input_t *input = data;
  return input->curr_char >= input->code_len ? EOF : input->code[input->curr_char++];
}

static void fancy_abort (void) {
  fprintf (stderr, "Test failed\n");
  abort ();
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

  for (size_t i = 0; i < sizeof (std_libs) / sizeof (struct lib); i++)
    if ((handler = std_libs[i].handler) != NULL && (sym = dlsym (handler, name)) != NULL) break;
  if (sym == NULL)
    for (size_t i = 0; i < VARR_LENGTH (lib_t, cmdline_libs); i++)
      if ((handler = VARR_GET (lib_t, cmdline_libs, i).handler) != NULL
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

static int mir_read_func (MIR_context_t ctx MIR_UNUSED) { return t_getc (&curr_input); }

static const char *get_file_name (const char *name, const char *suffix) {
  const char *res = strrchr (name, slash);

  if (res != NULL) name = res + 1;
  VARR_TRUNC (char, temp_string, 0);
  VARR_PUSH_ARR (char, temp_string, name,
                 (res = strrchr (name, '.')) == NULL ? strlen (name) : (size_t) (res - name));
  VARR_PUSH_ARR (char, temp_string, suffix, strlen (suffix) + 1); /* including zero byte */
  return VARR_ADDR (char, temp_string);
}

static FILE *get_output_file (const char *file_name) {
  FILE *f;
  if ((f = fopen (file_name, "wb")) != NULL) return f;
  fprintf (stderr, "cannot create file %s\n", file_name);
  exit (1);  // ???
}

static FILE *get_output_file_from_parts (const char **result_file_name, const char *out_file_name,
                                         const char *source_name, const char *suffix) {
  *result_file_name = out_file_name != NULL ? out_file_name : get_file_name (source_name, suffix);
  return get_output_file (*result_file_name);
}

#if C2MIR_PARALLEL
static void parallel_error (const char *message) {
  fprintf (stderr, "%s -- good bye\n", message);
  exit (1);
}
#endif

static MIR_context_t main_ctx;

struct compiler {
  MIR_context_t ctx;
  int num, busy_p;
  struct input input;
#if C2MIR_PARALLEL
  pthread_t compile_thread;
#endif
};
typedef struct compiler *compiler_t;

static struct compiler *compilers;

static int result_code;

#if C2MIR_PARALLEL

static mir_mutex_t queue_mutex;
static mir_cond_t compile_signal, done_signal;
static size_t inputs_start;

static void *compile (void *arg) {
  compiler_t compiler = arg;
  const char *result_file_name;
  MIR_context_t ctx = compiler->ctx;
  int error_p;
  size_t len;

  for (;;) {
    if (mir_mutex_lock (&queue_mutex)) parallel_error ("error in mutex lock");
    while (VARR_LENGTH (input_t, inputs_to_compile) <= inputs_start)
      if (mir_cond_wait (&compile_signal, &queue_mutex)) parallel_error ("error in cond wait");
    compiler->input = VARR_GET (input_t, inputs_to_compile, inputs_start);
    if (compiler->input.input_name == NULL) {
      if (mir_mutex_unlock (&queue_mutex)) parallel_error ("error in mutex unlock");
      break;
    }
    inputs_start++;
    if (inputs_start > 64 && VARR_LENGTH (input_t, inputs_to_compile) < 2 * inputs_start) {
      len = VARR_LENGTH (input_t, inputs_to_compile) - inputs_start;
      memmove (VARR_ADDR (input_t, inputs_to_compile), /* compact */
               VARR_ADDR (input_t, inputs_to_compile) + inputs_start, len * sizeof (input_t));
      VARR_TRUNC (input_t, inputs_to_compile, len);
      inputs_start = 0;
    }
    compiler->busy_p = TRUE;
    if (mir_mutex_unlock (&queue_mutex)) parallel_error ("error in mutex unlock");
    FILE *f = (!options.asm_p && !options.object_p
                 ? NULL
                 : get_output_file_from_parts (&result_file_name, options.output_file_name,
                                               compiler->input.input_name,
                                               options.asm_p ? ".mir" : ".bmir"));
    error_p = !c2mir_compile (ctx, &compiler->input.options, t_getc, &compiler->input,
                              compiler->input.input_name, f);
    if (mir_mutex_lock (&queue_mutex)) parallel_error ("error in mutex lock");
    compiler->busy_p = FALSE;
    if (compiler->input.code_container != NULL) {
      VARR_DESTROY (uint8_t, compiler->input.code_container);
      compiler->input.code_container = NULL;
    }
    if (error_p) result_code = 1;
    if (mir_cond_signal (&done_signal)) parallel_error ("error in cond signal");
    if (mir_mutex_unlock (&queue_mutex)) parallel_error ("error in mutex unlock");
  }
  return NULL;
}

#endif

static void init_compilers (void) {
  if (threads_num == 0) return;
#if C2MIR_PARALLEL
  if (mir_mutex_init (&queue_mutex, NULL) != 0) {
    fprintf (stderr, "can not create a c2m thread lock -- bye!\n");
    exit (1);
  } else if (mir_cond_init (&compile_signal, NULL) != 0) {
    fprintf (stderr, "can not create a c2m thread signal -- bye!\n");
    exit (1);
  } else if (mir_cond_init (&done_signal, NULL) != 0) {
    fprintf (stderr, "can not create a c2m thread signal -- bye!\n");
    exit (1);
  }
#endif
  compilers = reg_malloc (sizeof (struct compiler) * threads_num);
  for (int i = 0; i < threads_num; i++) {
    compiler_t compiler = &compilers[i];
    compiler->busy_p = FALSE;
    compiler->num = i;
    compiler->ctx = MIR_init ();
    c2mir_init (compiler->ctx);
#if C2MIR_PARALLEL
    mir_thread_attr_t attr;
    if (mir_thread_attr_init (&attr) != 0
        || mir_thread_attr_setstacksize (&attr, 2 * 1024 * 1024) != 0) {
      fprintf (stderr, "can not increase c2m thread stack size -- bye!\n");
      exit (1);
    }
    if (mir_thread_create (&compiler->compile_thread, &attr, compile, compiler) != 0) {
      fprintf (stderr, "can not create a c2m thread -- bye!\n");
      exit (1);
    }
#endif
  }
}

static void finish_compilers (void) {
  if (threads_num == 0) return;
#if C2MIR_PARALLEL
  if (mir_mutex_destroy (&queue_mutex) != 0 || mir_cond_destroy (&compile_signal) != 0
      || mir_cond_destroy (&done_signal) != 0) {  // ???
    parallel_error ("can not destroy compiler mutex or signals");
    exit (1);
  }
#endif
  for (int i = 0; i < threads_num; i++) {
    compiler_t compiler = &compilers[i];
    c2mir_finish (compiler->ctx);
    MIR_finish (compiler->ctx);
  }
}

#if C2MIR_PARALLEL
static void signal_compilers_to_finish (int cancel_p) {
  if (mir_mutex_lock (&queue_mutex)) parallel_error ("error in mutex lock");
  if (cancel_p) {
    inputs_start = 0;
    VARR_TRUNC (input_t, inputs_to_compile, 0);
  }
  curr_input.input_name = NULL; /* flag to finish threads */
  VARR_PUSH (input_t, inputs_to_compile, curr_input);
  if (mir_cond_broadcast (&compile_signal)) parallel_error ("error in cond broadcast");
  if (mir_mutex_unlock (&queue_mutex)) parallel_error ("error in mutex unlock");
}
#endif

static void send_to_compile (input_t *input) {
  if (input == NULL) { /* finish compilation */
#if C2MIR_PARALLEL
    if (threads_num >= 1) {
      signal_compilers_to_finish (FALSE);
      for (int i = 0; i < threads_num; i++) mir_thread_join (compilers[i].compile_thread, NULL);
    }
#endif
    return;
  }
  if (!C2MIR_PARALLEL || threads_num == 0) {
    const char *result_file_name;
    FILE *f;

    f = (!options.asm_p && !options.object_p
           ? NULL
           : get_output_file_from_parts (&result_file_name, options.output_file_name,
                                         input->input_name, options.asm_p ? ".mir" : ".bmir"));
    if (!c2mir_compile (main_ctx, &input->options, t_getc, input, input->input_name, f))
      result_code = 1;
    if (input->code_container != NULL) VARR_DESTROY (uint8_t, input->code_container);
    return;
  }
#if C2MIR_PARALLEL
  if (mir_mutex_lock (&queue_mutex)) parallel_error ("error in c2m mutex lock");
  VARR_PUSH (input_t, inputs_to_compile, *input);
  if (mir_cond_broadcast (&compile_signal)) parallel_error ("error in c2m cond broadcast");
  if (mir_mutex_unlock (&queue_mutex)) parallel_error ("error in c2m mutex unlock");
#endif
}

#if C2MIR_PARALLEL

static void move_modules_main_context (MIR_context_t ctx) {
  MIR_module_t module, next_module;

  for (module = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx)); module != NULL;
       module = next_module) {
    next_module = DLIST_NEXT (MIR_module_t, module);
    MIR_change_module_ctx (ctx, module, main_ctx);
  }
}

static int module_cmp (const void *a1, const void *a2) {
  MIR_module_t m1 = *(MIR_module_t *) a1, m2 = *(MIR_module_t *) a2;
  return strcmp (m1->name, m2->name);
}

DEF_VARR (MIR_module_t);

static void sort_modules (MIR_context_t ctx) {
  MIR_module_t module;
  DLIST (MIR_module_t) *list = MIR_get_module_list (ctx);
  VARR (MIR_module_t) * modules;

  VARR_CREATE (MIR_module_t, modules, &default_alloc, 16);
  while ((module = DLIST_HEAD (MIR_module_t, *list)) != NULL) {
    DLIST_REMOVE (MIR_module_t, *list, module);
    VARR_PUSH (MIR_module_t, modules, module);
  }
  qsort (VARR_ADDR (MIR_module_t, modules), VARR_LENGTH (MIR_module_t, modules),
         sizeof (MIR_module_t), module_cmp);
  for (size_t i = 0; i < VARR_LENGTH (MIR_module_t, modules); i++)
    DLIST_APPEND (MIR_module_t, *list, VARR_GET (MIR_module_t, modules, i));
  VARR_DESTROY (MIR_module_t, modules);
}
#endif

int main (int argc, char *argv[], char *env[]) {
  int i, bin_p;
  size_t len;

  interp_exec_p = gen_exec_p = lazy_gen_exec_p = lazy_bb_gen_exec_p = FALSE;
  VARR_CREATE (void_ptr_t, allocated, &default_alloc, 100);
  VARR_CREATE (char_ptr_t, source_file_names, &default_alloc, 32);
  VARR_CREATE (char_ptr_t, exec_argv, &default_alloc, 32);
  VARR_CREATE (lib_t, cmdline_libs, &default_alloc, 16);
  VARR_CREATE (char_ptr_t, lib_dirs, &default_alloc, 16);
  for (size_t n = 0; n < sizeof (std_lib_dirs) / sizeof (char_ptr_t); n++)
    VARR_PUSH (char_ptr_t, lib_dirs, std_lib_dirs[n]);
  VARR_CREATE (input_t, inputs_to_compile, &default_alloc, 32);
  options.prepro_output_file = NULL;
  init_options (argc, argv);
  main_ctx = MIR_init ();
  if (!C2MIR_PARALLEL || threads_num <= 0) c2mir_init (main_ctx);
  init_compilers ();
  result_code = 0;
  for (i = 0, options.module_num = 0;; i++, options.module_num++) {
    curr_input.input_name = NULL;
    if (i == 0) { /* check and modify options */
      if (curr_input.code == NULL && VARR_LENGTH (char_ptr_t, source_file_names) == 0) {
        fprintf (stderr, "No source file is given -- good bye.\n");
        exit (1);
      }
      if (curr_input.code != NULL && VARR_LENGTH (char_ptr_t, source_file_names) > 0) {
        fprintf (stderr, "-s and other sources on the command line -- good bye.\n");
        exit (1);
      }
      for (size_t j = 0; j < VARR_LENGTH (char_ptr_t, source_file_names); j++)
        if (strcmp (VARR_GET (char_ptr_t, source_file_names, j), STDIN_SOURCE_NAME) == 0
            && VARR_LENGTH (char_ptr_t, source_file_names) > 1) {
          fprintf (stderr, "-i and sources on the command line -- good bye.\n");
          exit (1);
        }
      if (options.output_file_name == NULL && options.prepro_only_p) {
        options.prepro_output_file = stdout;
      } else if (options.output_file_name != NULL) {
#if defined(__unix__) || defined(__APPLE__)
        if (VARR_LENGTH (char_ptr_t, source_file_names) == 1) {
          const char *source_name = VARR_GET (char_ptr_t, source_file_names, 0);

          if (strcmp (source_name, STDIN_SOURCE_NAME) != 0) {
            struct stat stat1, stat2;

            stat (source_name, &stat1);
            stat (options.output_file_name, &stat2);
            if (stat1.st_dev == stat2.st_dev && stat1.st_ino == stat2.st_ino) {
              fprintf (stderr, "-o %s will rewrite input source file %s -- good bye.\n",
                       options.output_file_name, source_name);
              exit (1);
            }
          }
        }
#endif
        if (options.prepro_only_p) {
          if (options.output_file_name != NULL
              && (options.prepro_output_file = fopen (options.output_file_name, "wb")) == NULL) {
            fprintf (stderr, "cannot create file %s -- good bye.\n", options.output_file_name);
            exit (1);
          }
        } else if ((options.asm_p || options.object_p)
                   && VARR_LENGTH (char_ptr_t, source_file_names) > 1) {
          fprintf (stderr, "-S or -c with -o for multiple files -- good bye.\n");
          exit (1);
        }
      }
    }
    curr_input.curr_char = 0;
    curr_input.code_container = NULL;
    if (curr_input.code != NULL) { /* command line script: */
      if (i > 0) break;
      curr_input.input_name = COMMAND_LINE_SOURCE_NAME;
    } else { /* stdin input or files given on the command line: */
      int c;
      FILE *f;

      if (i >= (int) VARR_LENGTH (char_ptr_t, source_file_names)) break;
      curr_input.input_name = VARR_GET (char_ptr_t, source_file_names, i);
      if (strcmp (curr_input.input_name, STDIN_SOURCE_NAME) == 0) {
        f = stdin;
      } else if ((f = fopen (curr_input.input_name, "rb")) == NULL) {
        fprintf (stderr, "can not open %s -- goodbye\n", curr_input.input_name);
        exit (1);
      }
      VARR_CREATE (uint8_t, curr_input.code_container, &default_alloc, 1000);
      while ((c = getc (f)) != EOF) VARR_PUSH (uint8_t, curr_input.code_container, c);
      curr_input.code_len = VARR_LENGTH (uint8_t, curr_input.code_container);
      VARR_PUSH (uint8_t, curr_input.code_container, 0);
      curr_input.code = VARR_ADDR (uint8_t, curr_input.code_container);
      fclose (f);
    }
    assert (curr_input.input_name != NULL);
    len = strlen (curr_input.input_name);
    if ((bin_p = len >= 5 && strcmp (curr_input.input_name + len - 5, ".bmir") == 0)
        || (len >= 4 && strcmp (curr_input.input_name + len - 4, ".mir") == 0)) {
      DLIST (MIR_module_t) *mlist = MIR_get_module_list (main_ctx);
      MIR_module_t m, last_m = DLIST_TAIL (MIR_module_t, *mlist);
      const char *result_file_name;
      FILE *f;

      if (bin_p) {
        MIR_read_with_func (main_ctx, mir_read_func);
      } else {
        curr_input.code_len++; /* include zero byte */
        MIR_scan_string (main_ctx, (char *) curr_input.code);
      }
      if (curr_input.code_container != NULL) VARR_DESTROY (uint8_t, curr_input.code_container);
      if (!options.prepro_only_p && !options.syntax_only_p
          && ((bin_p && !options.object_p && options.asm_p)
              || (!bin_p && !options.asm_p && options.object_p))) {
        f = get_output_file_from_parts (&result_file_name, options.output_file_name,
                                        curr_input.input_name, bin_p ? ".mir" : ".bmir");
        for (m = last_m == NULL ? DLIST_HEAD (MIR_module_t, *mlist)
                                : DLIST_NEXT (MIR_module_t, last_m);
             m != NULL; m = DLIST_NEXT (MIR_module_t, m))
          (bin_p ? MIR_output_module : MIR_write_module) (main_ctx, f, m);
        if (ferror (f) || fclose (f)) {
          fprintf (stderr, "error in writing file %s\n", result_file_name);
          result_code = 1;
        }
      }
    } else {
      curr_input.options = options;
      send_to_compile (&curr_input);
    }
    curr_input.code = NULL; /* no cmd line input anymore */
  }
  send_to_compile (NULL);
  if (options.prepro_output_file != NULL
      && (ferror (options.prepro_output_file)
          || (options.prepro_output_file != stdout && fclose (options.prepro_output_file)))) {
    fprintf (stderr, "error in writing to file %s\n", options.output_file_name);
    result_code = 1;
  }
  if (result_code == 0 && !options.prepro_only_p && !options.syntax_only_p && !options.asm_p
      && !options.object_p) {
    MIR_val_t val;
    MIR_module_t module;
    MIR_item_t func, main_func = NULL;
    uint64_t (*fun_addr) (int, void *argv, char *env[]);
    double start_time;

#if C2MIR_PARALLEL
    if (threads_num > 0) {
      for (i = 0; i < threads_num; i++) move_modules_main_context (compilers[i].ctx);
      sort_modules (main_ctx);
    }
#endif
    for (module = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (main_ctx)); module != NULL;
         module = DLIST_NEXT (MIR_module_t, module)) {
      for (func = DLIST_HEAD (MIR_item_t, module->items); func != NULL;
           func = DLIST_NEXT (MIR_item_t, func))
        if (func->item_type == MIR_func_item && strcmp (func->u.func->name, "main") == 0)
          main_func = func;
      MIR_load_module (main_ctx, module);
    }
    if (main_func == NULL) {
      fprintf (stderr, "cannot link program w/o main function\n");
      result_code = 1;
    } else if (!interp_exec_p && !gen_exec_p && !lazy_gen_exec_p && !lazy_bb_gen_exec_p) {
      const char *file_name
        = options.output_file_name == NULL ? "a.bmir" : options.output_file_name;
      FILE *f = fopen (file_name, "wb");

      if (f == NULL) {
        fprintf (stderr, "cannot open file %s\n", file_name);
        result_code = 1;
      } else {
        start_time = real_usec_time ();
        MIR_write (main_ctx, f);
        if (ferror (f) || fclose (f)) {
          fprintf (stderr, "error in writing file %s\n", file_name);
          result_code = 1;
        } else if (options.verbose_p) {
          fprintf (stderr, "binary output      -- %.0f msec\n",
                   (real_usec_time () - start_time) / 1000.0);
        }
      }
    } else {
      open_std_libs ();
      MIR_load_external (main_ctx, "abort", fancy_abort);
      MIR_load_external (main_ctx, "_MIR_flush_code_cache", _MIR_flush_code_cache);
      start_time = real_usec_time ();
      if (interp_exec_p) {
        if (options.verbose_p)
          fprintf (stderr, "MIR link interp start  -- %.0f usec\n", real_usec_time () - start_time);
        MIR_link (main_ctx, MIR_set_interp_interface, import_resolver);
        if (options.verbose_p)
          fprintf (stderr, "MIR Link finish        -- %.0f usec\n", real_usec_time () - start_time);
        start_time = real_usec_time ();
        MIR_interp (main_ctx, main_func, &val, 3,
                    (MIR_val_t){.i = VARR_LENGTH (char_ptr_t, exec_argv)},
                    (MIR_val_t){.a = (void *) VARR_ADDR (char_ptr_t, exec_argv)},
                    (MIR_val_t){.a = (void *) env});
        result_code = (int) val.i;
        if (options.verbose_p) {
          fprintf (stderr, "  execution       -- %.0f usec\n", real_usec_time () - start_time);
          fprintf (stderr, "exit code: %lu\n", (long unsigned) result_code);
        }
      } else {
        int fun_argc = (int) VARR_LENGTH (char_ptr_t, exec_argv);
        const char **fun_argv = VARR_ADDR (char_ptr_t, exec_argv);

        if (options.verbose_p)
          fprintf (stderr, "MIR gen init start         -- %.0f usec\n",
                   real_usec_time () - start_time);
        MIR_gen_init (main_ctx);
        if (options.verbose_p)
          fprintf (stderr, "MIR gen init finish         -- %.0f usec\n",
                   real_usec_time () - start_time);
        if (optimize_level >= 0) MIR_gen_set_optimize_level (main_ctx, (unsigned) optimize_level);
        if (gen_debug_level >= 0) {
          MIR_gen_set_debug_file (main_ctx, stderr);
          MIR_gen_set_debug_level (main_ctx, gen_debug_level);
        }
        MIR_link (main_ctx,
                  gen_exec_p        ? MIR_set_gen_interface
                  : lazy_gen_exec_p ? MIR_set_lazy_gen_interface
                                    : MIR_set_lazy_bb_gen_interface,
                  import_resolver);
        if (options.verbose_p)
          fprintf (stderr, "MIR link finish        -- %.0f usec\n", real_usec_time () - start_time);
        fun_addr = main_func->addr;
        start_time = real_usec_time ();
        result_code = (int) fun_addr (fun_argc, fun_argv, env);
        if (options.verbose_p) {
          fprintf (stderr, "  execution       -- %.0f msec\n",
                   (real_usec_time () - start_time) / 1000.0);
          fprintf (stderr, "exit code: %d\n", result_code);
        }
        MIR_gen_finish (main_ctx);
      }
    }
  }
  close_cmdline_libs ();
  close_std_libs ();
  finish_compilers ();
  if (!C2MIR_PARALLEL || threads_num <= 0) c2mir_finish (main_ctx);
  MIR_finish (main_ctx);
  VARR_DESTROY (char, temp_string);
  VARR_DESTROY (char_ptr_t, headers);
  VARR_DESTROY (macro_command_t, macro_commands);
  VARR_DESTROY (char_ptr_t, lib_dirs);
  VARR_DESTROY (lib_t, cmdline_libs);
  VARR_DESTROY (char_ptr_t, source_file_names);
  VARR_DESTROY (char_ptr_t, exec_argv);
  VARR_DESTROY (input_t, inputs_to_compile);
  for (size_t n = 0; n < VARR_LENGTH (void_ptr_t, allocated); n++)
    MIR_free (&default_alloc, VARR_GET (void_ptr_t, allocated, n));
  VARR_DESTROY (void_ptr_t, allocated);
  return result_code;
}
