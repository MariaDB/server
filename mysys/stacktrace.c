/*
   Copyright (c) 2001, 2011, Oracle and/or its affiliates
   Copyright (c) 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "mysys_priv.h"
#include <my_stacktrace.h>

#ifndef __WIN__
#include <signal.h>
#include <m_string.h>
#ifdef HAVE_STACKTRACE
#include <unistd.h>
#include <strings.h>

#ifdef __linux__
#include <ctype.h>          /* isprint */
#include <sys/syscall.h>    /* SYS_gettid */
#endif

#if HAVE_EXECINFO_H
#include <execinfo.h>
#endif

/**
   Default handler for printing stacktrace
*/

static sig_handler default_handle_fatal_signal(int sig)
{
  my_safe_printf_stderr("%s: Got signal %d. Attempting backtrace\n",
                        my_progname_short, sig);
  my_print_stacktrace(0,0,1);
#ifndef __WIN__
  signal(sig, SIG_DFL);
  kill(getpid(), sig);
#endif /* __WIN__ */
  return;
}


/**
   Initialize priting off stacktrace at signal
*/

void my_setup_stacktrace(void)
{
  struct sigaction sa;
  sa.sa_flags = SA_RESETHAND | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  sa.sa_handler= default_handle_fatal_signal;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
#ifdef SIGBUS
  sigaction(SIGBUS, &sa, NULL);
#endif
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
}


/*
  Attempt to print a char * pointer as a string.

  SYNOPSIS
    Prints either until the end of string ('\0'), or max_len characters have
    been printed.

  RETURN VALUE
    0  Pointer was within the heap address space.
       The string was printed fully, or until the end of the heap address space.
    1  Pointer is outside the heap address space. Printed as invalid.

  NOTE
    On some systems, we can have valid pointers outside the heap address space.
    This is through the use of mmap inside malloc calls. When this function
    returns 1, it does not mean 100% that the pointer is corrupted.
*/

int my_safe_print_str(const char* val, size_t max_len)
{
  const char *orig_val= val;
  if (!val)
  {
    my_safe_printf_stderr("%s", "(null)");
    return 1;
  }

  for (; max_len; --max_len)
  {
    if (my_write_stderr((val++), 1) != 1)
    {
      if ((errno == EFAULT) &&(val == orig_val + 1))
      {
        // We can not read the address from very beginning
        my_safe_printf_stderr("Can't access address %p", orig_val);
      }
      break;
    }
  }
  my_safe_printf_stderr("%s", "\n");

  return 0;
}

#if defined(HAVE_PRINTSTACK)

/* Use Solaris' symbolic stack trace routine. */
#include <ucontext.h>

void my_print_stacktrace(uchar* stack_bottom __attribute__((unused)), 
                         ulong thread_stack __attribute__((unused)),
                         my_bool silent)
{
  if (printstack(fileno(stderr)) == -1)
    my_safe_printf_stderr("%s",
      "Error when traversing the stack, stack appears corrupt.\n");
  else if (!silent)
    my_safe_printf_stderr("%s",
      "Please read "
      "http://dev.mysql.com/doc/refman/5.1/en/resolve-stack-dump.html\n"
      "and follow instructions on how to resolve the stack trace.\n"
      "Resolved stack trace is much more helpful in diagnosing the\n"
      "problem, so please do resolve it\n");
}

#elif HAVE_BACKTRACE && (HAVE_BACKTRACE_SYMBOLS || HAVE_BACKTRACE_SYMBOLS_FD)

#if BACKTRACE_DEMANGLE

char __attribute__ ((weak)) *
my_demangle(const char *mangled_name __attribute__((unused)),
            int *status __attribute__((unused)))
{
  return NULL;
}

static void my_demangle_symbols(char **addrs, int n)
{
  int status, i;
  char *begin, *end, *demangled;

  for (i= 0; i < n; i++)
  {
    demangled= NULL;
    begin= strchr(addrs[i], '(');
    end= begin ? strchr(begin, '+') : NULL;

    if (begin && end)
    {
      *begin++= *end++= '\0';
      demangled= my_demangle(begin, &status);
      if (!demangled || status)
      {
        demangled= NULL;
        begin[-1]= '(';
        end[-1]= '+';
      }
    }

    if (demangled)
      my_safe_printf_stderr("%s(%s+%s\n", addrs[i], demangled, end);
    else
      my_safe_printf_stderr("%s\n", addrs[i]);
  }
}

#endif /* BACKTRACE_DEMANGLE */

#if HAVE_MY_ADDR_RESOLVE
static int print_with_addr_resolve(void **addrs, int n)
{
  int i;
  const char *err;

  if ((err= my_addr_resolve_init()))
  {
    my_safe_printf_stderr("(my_addr_resolve failure: %s)\n", err);
    return 0;
  }

  for (i= 0; i < n; i++)
  {
    my_addr_loc loc;
    if (my_addr_resolve(addrs[i], &loc))
      backtrace_symbols_fd(addrs+i, 1, fileno(stderr));
    else
      my_safe_printf_stderr("%s:%u(%s)[%p]\n",
              loc.file, loc.line, loc.func, addrs[i]);
  }
  return 1;
}
#endif

void my_print_stacktrace(uchar* stack_bottom, ulong thread_stack,
                         my_bool silent __attribute__((unused)))
{
  void *addrs[128];
  char **strings __attribute__((unused)) = NULL;
  int n = backtrace(addrs, array_elements(addrs));
  my_safe_printf_stderr("stack_bottom = %p thread_stack 0x%lx\n",
                        stack_bottom, thread_stack);
#if HAVE_MY_ADDR_RESOLVE
  if (print_with_addr_resolve(addrs, n))
    return;
#endif
#if BACKTRACE_DEMANGLE
  if ((strings= backtrace_symbols(addrs, n)))
  {
    my_demangle_symbols(strings, n);
    free(strings);
    return;
  }
#endif
#if HAVE_BACKTRACE_SYMBOLS_FD
  backtrace_symbols_fd(addrs, n, fileno(stderr));
#endif
}

#elif defined(TARGET_OS_LINUX)

#ifdef __i386__
#define SIGRETURN_FRAME_OFFSET 17
#endif

#ifdef __x86_64__
#define SIGRETURN_FRAME_OFFSET 23
#endif

#if defined(__alpha__) && defined(__GNUC__)
/*
  The only way to backtrace without a symbol table on alpha
  is to find stq fp,N(sp), and the first byte
  of the instruction opcode will give us the value of N. From this
  we can find where the old value of fp is stored
*/

#define MAX_INSTR_IN_FUNC  10000

inline uchar** find_prev_fp(uint32* pc, uchar** fp)
{
  int i;
  for (i = 0; i < MAX_INSTR_IN_FUNC; ++i,--pc)
  {
    uchar* p = (uchar*)pc;
    if (p[2] == 222 &&  p[3] == 35)
    {
      return (uchar**)((uchar*)fp - *(short int*)p);
    }
  }
  return 0;
}

inline uint32* find_prev_pc(uint32* pc, uchar** fp)
{
  int i;
  for (i = 0; i < MAX_INSTR_IN_FUNC; ++i,--pc)
  {
    char* p = (char*)pc;
    if (p[1] == 0 && p[2] == 94 &&  p[3] == -73)
    {
      uint32* prev_pc = (uint32*)*((fp+p[0]/sizeof(fp)));
      return prev_pc;
    }
  }
  return 0;
}
#endif /* defined(__alpha__) && defined(__GNUC__) */

void my_print_stacktrace(uchar* stack_bottom, ulong thread_stack,
                         my_bool silent)
{
  uchar** UNINIT_VAR(fp);
  uint frame_count = 0, sigreturn_frame_count;
#if defined(__alpha__) && defined(__GNUC__)
  uint32* pc;
#endif


#ifdef __i386__
  __asm __volatile__ ("movl %%ebp,%0"
		      :"=r"(fp)
		      :"r"(fp));
#endif
#ifdef __x86_64__
  __asm __volatile__ ("movq %%rbp,%0"
		      :"=r"(fp)
		      :"r"(fp));
#endif
#if defined(__alpha__) && defined(__GNUC__) 
  __asm __volatile__ ("mov $30,%0"
		      :"=r"(fp)
		      :"r"(fp));
#endif
  if (!fp)
  {
    my_safe_printf_stderr("%s",
      "frame pointer is NULL, did you compile with\n"
      "-fomit-frame-pointer? Aborting backtrace!\n");
    return;
  }

  if (!stack_bottom || (uchar*) stack_bottom > (uchar*) &fp)
  {
    ulong tmp= MY_MIN(0x10000,thread_stack);
    /* Assume that the stack starts at the previous even 65K */
    stack_bottom= (uchar*) (((ulong) &fp + tmp) & ~(ulong) 0xFFFF);
    my_safe_printf_stderr("Cannot determine thread, fp=%p, "
                          "backtrace may not be correct.\n", fp);
  }
  if (fp > (uchar**) stack_bottom ||
      fp < (uchar**) stack_bottom - thread_stack)
  {
    my_safe_printf_stderr("Bogus stack limit or frame pointer, "
                          "fp=%p, stack_bottom=%p, thread_stack=%ld, "
                          "aborting backtrace.\n",
                          fp, stack_bottom, thread_stack);
    return;
  }

  my_safe_printf_stderr("%s",
    "Stack range sanity check OK, backtrace follows:\n");
#if defined(__alpha__) && defined(__GNUC__)
  my_safe_printf_stderr("%s",
    "Warning: Alpha stacks are difficult -"
    "will be taking some wild guesses, stack trace may be incorrect or "
    "terminate abruptly\n");

  /* On Alpha, we need to get pc */
  __asm __volatile__ ("bsr %0, do_next; do_next: "
		      :"=r"(pc)
		      :"r"(pc));
#endif  /* __alpha__ */

  /* We are 1 frame above signal frame with NPTL */
  sigreturn_frame_count = 1;

  while (fp < (uchar**) stack_bottom)
  {
#if defined(__i386__) || defined(__x86_64__)
    uchar** new_fp = (uchar**)*fp;
    my_safe_printf_stderr("%p\n",
                          frame_count == sigreturn_frame_count ?
                          *(fp + SIGRETURN_FRAME_OFFSET) : *(fp + 1));
#endif /* defined(__386__)  || defined(__x86_64__) */

#if defined(__alpha__) && defined(__GNUC__)
    uchar** new_fp = find_prev_fp(pc, fp);
    if (frame_count == sigreturn_frame_count - 1)
    {
      new_fp += 90;
    }

    if (fp && pc)
    {
      pc = find_prev_pc(pc, fp);
      if (pc)
	my_safe_printf_stderr("%p\n", pc);
      else
      {
        my_safe_printf_stderr("%s",
          "Not smart enough to deal with the rest of this stack\n");
	goto end;
      }
    }
    else
    {
      my_safe_printf_stderr("%s",
        "Not smart enough to deal with the rest of this stack\n");
      goto end;
    }
#endif /* defined(__alpha__) && defined(__GNUC__) */
    if (new_fp <= fp )
    {
      my_safe_printf_stderr("New value of fp=%p failed sanity check, "
                            "terminating stack trace!\n", new_fp);
      goto end;
    }
    fp = new_fp;
    ++frame_count;
  }
  my_safe_printf_stderr("%s",
                        "Stack trace seems successful - bottom reached\n");

end:
  if (!silent)
    my_safe_printf_stderr("%s",
    "Please read "
    "http://dev.mysql.com/doc/refman/5.1/en/resolve-stack-dump.html\n"
    "and follow instructions on how to resolve the stack trace.\n"
    "Resolved stack trace is much more helpful in diagnosing the\n"
    "problem, so please do resolve it\n");
}
#endif /* TARGET_OS_LINUX */
#endif /* HAVE_STACKTRACE */

/* Produce a core for the thread */
void my_write_core(int sig)
{
#ifdef HAVE_gcov
  extern void __gcov_flush(void);
#endif
  signal(sig, SIG_DFL);
#ifdef HAVE_gcov
  /*
    For GCOV build, crashing will prevent the writing of code coverage
    information from this process, causing gcov output to be incomplete.
    So we force the writing of coverage information here before terminating.
  */
  __gcov_flush();
#endif
  pthread_kill(pthread_self(), sig);
#if defined(P_MYID) && !defined(SCO)
  /* On Solaris, the above kill is not enough */
  sigsend(P_PID,P_MYID,sig);
#endif
}

#else /* __WIN__*/

#ifdef _MSC_VER
/* Silence warning in OS header dbghelp.h */
#pragma warning(push)
#pragma warning(disable : 4091)
#endif

#include <dbghelp.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <tlhelp32.h>
#include <my_sys.h>
#if _MSC_VER
#pragma comment(lib, "dbghelp")
#endif

static EXCEPTION_POINTERS *exception_ptrs;

#define MODULE64_SIZE_WINXP 576
#define STACKWALK_MAX_FRAMES 64

void my_set_exception_pointers(EXCEPTION_POINTERS *ep)
{
  exception_ptrs = ep;
}

/*
  Appends directory to symbol path.
*/
static void add_to_symbol_path(char *path, size_t path_buffer_size, 
  char *dir, size_t dir_buffer_size)
{
  strcat_s(dir, dir_buffer_size, ";");
  if (!strstr(path, dir))
  {
    strcat_s(path, path_buffer_size, dir);
  }
}

/*
  Get symbol path - semicolon-separated list of directories to search
  for debug symbols. We expect PDB in the same directory as
  corresponding exe or dll, so the path is build from directories of
  the loaded modules. If environment variable _NT_SYMBOL_PATH is set,
  it's value appended to the symbol search path
*/
static void get_symbol_path(char *path, size_t size)
{ 
  HANDLE hSnap; 
  char *envvar;
  char *p;
#ifndef DBUG_OFF
  static char pdb_debug_dir[MAX_PATH + 7];
#endif

  path[0]= '\0';

#ifndef DBUG_OFF
  /* 
    Add "debug" subdirectory of the application directory, sometimes PDB will 
    placed here by installation.
  */
  GetModuleFileName(NULL, pdb_debug_dir, MAX_PATH);
  p= strrchr(pdb_debug_dir, '\\');
  if(p)
  { 
    *p= 0;
    strcat_s(pdb_debug_dir, sizeof(pdb_debug_dir), "\\debug;");
    add_to_symbol_path(path, size, pdb_debug_dir, sizeof(pdb_debug_dir));
  }
#endif

  /*
    Enumerate all modules, and add their directories to the path.
    Avoid duplicate entries.
  */
  hSnap= CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (hSnap != INVALID_HANDLE_VALUE)
  {
    BOOL ret;
    MODULEENTRY32 mod;
    mod.dwSize= sizeof(MODULEENTRY32);
    for (ret= Module32First(hSnap, &mod); ret; ret= Module32Next(hSnap, &mod))
    {
      char *module_dir= mod.szExePath;
      p= strrchr(module_dir,'\\');
      if (!p)
      {
        /*
          Path separator was not found. Not known to happen, if ever happens,
          will indicate current directory.
        */
        module_dir[0]= '.';
        module_dir[1]= '\0';
      }
      else
      {
        *p= '\0';
      }
      add_to_symbol_path(path, size, module_dir,sizeof(mod.szExePath));
    }
    CloseHandle(hSnap);
  }

  
  /* Add _NT_SYMBOL_PATH, if present. */
  envvar= getenv("_NT_SYMBOL_PATH");
  if(envvar)
  {
    strcat_s(path, size, envvar);
  }
}

#define MAX_SYMBOL_PATH 32768

/* Platform SDK in VS2003 does not have definition for SYMOPT_NO_PROMPTS*/
#ifndef SYMOPT_NO_PROMPTS
#define SYMOPT_NO_PROMPTS 0
#endif

void my_print_stacktrace(uchar* unused1, ulong unused2, my_bool silent)
{
  HANDLE  hProcess= GetCurrentProcess();
  HANDLE  hThread= GetCurrentThread();
  static  IMAGEHLP_MODULE64 module= {sizeof(module)};
  static  IMAGEHLP_SYMBOL64_PACKAGE package;
  DWORD64 addr;
  DWORD   machine;
  int     i;
  CONTEXT context;
  STACKFRAME64 frame={0};
  static char symbol_path[MAX_SYMBOL_PATH];

  if(!exception_ptrs)
    return;

  /* Copy context, as stackwalking on original will unwind the stack */
  context = *(exception_ptrs->ContextRecord);
  /*Initialize symbols.*/
  SymSetOptions(SYMOPT_LOAD_LINES|SYMOPT_NO_PROMPTS|SYMOPT_DEFERRED_LOADS|SYMOPT_DEBUG);
  get_symbol_path(symbol_path, sizeof(symbol_path));
  SymInitialize(hProcess, symbol_path, TRUE);

  /*Prepare stackframe for the first StackWalk64 call*/
  frame.AddrFrame.Mode= frame.AddrPC.Mode= frame.AddrStack.Mode= AddrModeFlat;
#if (defined _M_IX86)
  machine= IMAGE_FILE_MACHINE_I386;
  frame.AddrFrame.Offset= context.Ebp;
  frame.AddrPC.Offset=    context.Eip;
  frame.AddrStack.Offset= context.Esp;
#elif (defined _M_X64)
  machine = IMAGE_FILE_MACHINE_AMD64;
  frame.AddrFrame.Offset= context.Rbp;
  frame.AddrPC.Offset=    context.Rip;
  frame.AddrStack.Offset= context.Rsp;
#else
  /*There is currently no need to support IA64*/
#pragma error ("unsupported architecture")
#endif

  package.sym.SizeOfStruct= sizeof(package.sym);
  package.sym.MaxNameLength= sizeof(package.name);

  /*Walk the stack, output useful information*/ 
  for(i= 0; i< STACKWALK_MAX_FRAMES;i++)
  {
    DWORD64 function_offset= 0;
    DWORD line_offset= 0;
    IMAGEHLP_LINE64 line= {sizeof(line)};
    BOOL have_module= FALSE;
    BOOL have_symbol= FALSE;
    BOOL have_source= FALSE;

    if(!StackWalk64(machine, hProcess, hThread, &frame, &context, 0, 0, 0 ,0))
      break;
    addr= frame.AddrPC.Offset;

    have_module= SymGetModuleInfo64(hProcess,addr,&module);
#ifdef _M_IX86
    if(!have_module)
    {
      /*
        ModuleInfo structure has been "compatibly" extended in
        releases after XP, and its size was increased. To make XP
        dbghelp.dll function happy, pretend passing the old structure.
      */
      module.SizeOfStruct= MODULE64_SIZE_WINXP;
      have_module= SymGetModuleInfo64(hProcess, addr, &module);
    }
#endif

    have_symbol= SymGetSymFromAddr64(hProcess, addr, &function_offset,
      &(package.sym));
    have_source= SymGetLineFromAddr64(hProcess, addr, &line_offset, &line);

    if(have_module)
    {
      const char *base_image_name= my_basename(module.ImageName);
      my_safe_printf_stderr("%s!", base_image_name);
    }
    if(have_symbol)
      my_safe_printf_stderr("%s()", package.sym.Name);

    else if(have_module)
      my_safe_printf_stderr("%s", "???");

    if(have_source)
    {
      const char *base_file_name= my_basename(line.FileName);
      my_safe_printf_stderr("[%s:%lu]",
                            base_file_name, line.LineNumber);
    }
    my_safe_printf_stderr("%s", "\n");
  }
}


/*
  Write dump. The dump is created in current directory,
  file name is constructed from executable name plus
  ".dmp" extension
*/
void my_write_core(int unused)
{
  char path[MAX_PATH];
  char dump_fname[MAX_PATH]= "core.dmp";
  MINIDUMP_EXCEPTION_INFORMATION info;
  HANDLE hFile;

  if(!exception_ptrs)
    return;

  info.ExceptionPointers= exception_ptrs;
  info.ClientPointers= FALSE;
  info.ThreadId= GetCurrentThreadId();

  if(GetModuleFileName(NULL, path, sizeof(path)))
  {
    _splitpath(path, NULL, NULL,dump_fname,NULL);
    strcat_s(dump_fname, sizeof(dump_fname), ".dmp");
  }

  hFile= CreateFile(dump_fname, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL, 0);
  if(hFile)
  {
    /* Create minidump */
    if(MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
      hFile, MiniDumpNormal, &info, 0, 0))
    {
      my_safe_printf_stderr("Minidump written to %s\n",
                            _fullpath(path, dump_fname, sizeof(path)) ?
                            path : dump_fname);
    }
    else
    {
      my_safe_printf_stderr("MiniDumpWriteDump() failed, last error %u\n",
                            (uint) GetLastError());
    }
    CloseHandle(hFile);
  }
  else
  {
    my_safe_printf_stderr("CreateFile(%s) failed, last error %u\n",
                          dump_fname, (uint) GetLastError());
  }
}


int my_safe_print_str(const char *val, size_t len)
{
  __try
  {
    my_write_stderr(val, len);
  }
  __except(EXCEPTION_EXECUTE_HANDLER)
  {
    my_safe_printf_stderr("%s", "is an invalid string pointer");
  }
  return 0;
}
#endif /*__WIN__*/


size_t my_write_stderr(const void *buf, size_t count)
{
  return (size_t) write(fileno(stderr), buf, (uint)count);
}


size_t my_safe_printf_stderr(const char* fmt, ...)
{
  char to[512];
  size_t result;
  va_list args;
  va_start(args,fmt);
  result= my_vsnprintf(to, sizeof(to), fmt, args);
  va_end(args);
  my_write_stderr(to, result);
  return result;
}
