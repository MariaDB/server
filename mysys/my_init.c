/*
   Copyright (c) 2000, 2012, Oracle and/or its affiliates
   Copyright (c) 2009, 2011, Monty Program Ab

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
#include "my_static.h"
#include "mysys_err.h"
#include <m_string.h>
#include <m_ctype.h>
#include <signal.h>
#include <mysql/psi/mysql_stage.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef _WIN32
#ifdef _MSC_VER
#include <locale.h>
#include <crtdbg.h>
/* WSAStartup needs winsock library*/
#pragma comment(lib, "ws2_32")
#endif
static void my_win_init(void);
static my_bool win32_init_tcp_ip();
static void setup_codepages();
#else
#define my_win_init()
#endif

#if defined(_SC_PAGE_SIZE) && !defined(_SC_PAGESIZE)
#define _SC_PAGESIZE _SC_PAGE_SIZE
#endif

extern pthread_key(struct st_my_thread_var*, THR_KEY_mysys);

#define SCALE_SEC       100
#define SCALE_USEC      10000

my_bool my_init_done= 0;
uint	mysys_usage_id= 0;              /* Incremented for each my_init() */
size_t  my_system_page_size= 8192;	/* Default if no sysconf() */

ulonglong   my_thread_stack_size= (sizeof(void*) <= 4)? 65536: ((256-16)*1024);

static ulong atoi_octal(const char *str)
{
  long int tmp;
  while (*str && my_isspace(&my_charset_latin1, *str))
    str++;
  str2int(str,
	  (*str == '0' ? 8 : 10),       /* Octalt or decimalt */
	  0, INT_MAX, &tmp);
  return (ulong) tmp;
}

MYSQL_FILE *mysql_stdin= NULL;
static MYSQL_FILE instrumented_stdin;

#ifdef _WIN32
static UINT orig_console_cp, orig_console_output_cp;

static void reset_console_cp(void)
{
  /*
    We try not to call SetConsoleCP unnecessarily, to workaround a bug on
    older Windows 10 (1803), which could switch truetype console fonts to
    raster, eventhough SetConsoleCP would be a no-op (switch from UTF8 to UTF8).
  */
  if (GetConsoleCP() != orig_console_cp)
    SetConsoleCP(orig_console_cp);
  if (GetConsoleOutputCP() != orig_console_output_cp)
    SetConsoleOutputCP(orig_console_output_cp);
}

/*
  The below fixes discrepancies in console output and
  command line parameter encoding. command line is in
  ANSI codepage, output to console by default is in OEM, but
  we like them to be in the same encoding.

  We do this only if current codepage is UTF8, i.e when we
  know we're on Windows that can handle UTF8 well.
*/
static void setup_codepages()
{
  UINT acp;
  BOOL is_a_tty= fileno(stdout) >= 0 && isatty(fileno(stdout));

  if (is_a_tty)
  {
    /*
      Save console codepages, in case we change them,
      to restore them on exit.
    */
    orig_console_cp= GetConsoleCP();
    orig_console_output_cp= GetConsoleOutputCP();
    if (orig_console_cp && orig_console_output_cp)
      atexit(reset_console_cp);
  }

  if ((acp= GetACP()) != CP_UTF8)
    return;

  /*
    Use setlocale to make mbstowcs/mkdir/getcwd behave, see
    https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/setlocale-wsetlocale
  */
  setlocale(LC_ALL, "en_US.UTF8");

  if (is_a_tty && (orig_console_cp != acp || orig_console_output_cp != acp))
  {
    /*
      If ANSI codepage is UTF8, we actually want to switch console
      to it as well.
    */
    SetConsoleCP(acp);
    SetConsoleOutputCP(acp);
  }
}
#endif

/**
  Initialize my_sys functions, resources and variables

  @return Initialization result
    @retval 0 Success
    @retval 1 Error. Couldn't initialize environment
*/
my_bool my_init(void)
{
  char *str;

  if (my_init_done)
    return 0;

  my_init_done= 1;

  mysys_usage_id++;
  my_umask= 0660;                       /* Default umask for new files */
  my_umask_dir= 0700;                   /* Default umask for new directories */
  my_global_flags= 0;
#ifdef _SC_PAGESIZE
  my_system_page_size= sysconf(_SC_PAGESIZE);
#endif

  /* Default creation of new files */
  if ((str= getenv("UMASK")) != 0)
    my_umask= (int) (atoi_octal(str) | 0600);
  /* Default creation of new dir's */
  if ((str= getenv("UMASK_DIR")) != 0)
    my_umask_dir= (int) (atoi_octal(str) | 0700);

  init_glob_errs();

  instrumented_stdin.m_file= stdin;
  instrumented_stdin.m_psi= NULL;       /* not yet instrumented */
  mysql_stdin= & instrumented_stdin;

  my_progname_short= "unknown";
  if (my_progname)
    my_progname_short= my_progname + dirname_length(my_progname);

  /* Initialize our mutex handling */
  my_mutex_init();

  if (my_thread_global_init())
    return 1;

#if defined(SAFEMALLOC) && !defined(DBUG_OFF)
  dbug_sanity= sf_sanity;
#endif

  /* $HOME is needed early to parse configuration files located in ~/ */
  if ((home_dir= getenv("HOME")) != 0)
    home_dir= intern_filename(home_dir_buff, home_dir);

  {
    DBUG_ENTER("my_init");
    DBUG_PROCESS((char*) (my_progname ? my_progname : "unknown"));
    my_time_init();
    my_win_init();
    DBUG_PRINT("exit", ("home: '%s'", home_dir));
#ifdef _WIN32
    if (win32_init_tcp_ip())
      DBUG_RETURN(1);
#endif
#ifdef CHECK_UNLIKELY
    init_my_likely();
#endif
    DBUG_RETURN(0);
  }
} /* my_init */


	/* End my_sys */

void my_end(int infoflag)
{
  /*
    this code is suboptimal to workaround a bug in
    Sun CC: Sun C++ 5.6 2004/06/02 for x86, and should not be
    optimized until this compiler is not in use anymore
  */
  FILE *info_file= DBUG_FILE;
  my_bool print_info= (info_file != stderr);

  if (!my_init_done)
    return;

  /*
    We do not use DBUG_ENTER here, as after cleanup DBUG is no longer
    operational, so we cannot use DBUG_RETURN.
  */
  DBUG_PRINT("info",("Shutting down: infoflag: %d  print_info: %d",
                     infoflag, print_info));
  if (!info_file)
  {
    info_file= stderr;
    print_info= 0;
  }

  if ((infoflag & MY_CHECK_ERROR) || print_info)
  {                                     /* Test if some file is left open */
    char ebuff[512];
    uint i, open_files, open_streams;

    for (open_streams= open_files= i= 0 ; i < my_file_limit ; i++)
    {
      if (my_file_info[i].type == UNOPEN)
        continue;
      if (my_file_info[i].type == STREAM_BY_FOPEN ||
          my_file_info[i].type == STREAM_BY_FDOPEN)
        open_streams++;
      else
        open_files++;

#ifdef EXTRA_DEBUG
      fprintf(stderr, EE(EE_FILE_NOT_CLOSED), my_file_info[i].name, i);
      fputc('\n', stderr);
#endif
    }
    if (open_files || open_streams)
    {
      my_snprintf(ebuff, sizeof(ebuff), EE(EE_OPEN_WARNING),
                  open_files, open_streams);
      my_message_stderr(EE_OPEN_WARNING, ebuff, ME_BELL);
      DBUG_PRINT("error", ("%s", ebuff));
    }

#ifdef CHECK_UNLIKELY
    end_my_likely(info_file);
#endif
  }
  free_charsets();
  my_error_unregister_all();
  my_once_free();

  if ((infoflag & MY_GIVE_INFO) || print_info)
  {
#ifdef HAVE_GETRUSAGE
    struct rusage rus;
#ifdef HAVE_valgrind
    /* Purify assumes that rus is uninitialized after getrusage call */
    bzero((char*) &rus, sizeof(rus));
#endif
    if (!getrusage(RUSAGE_SELF, &rus))
      fprintf(info_file,"\n\
User time %.2f, System time %.2f\n\
Maximum resident set size %ld, Integral resident set size %ld\n\
Non-physical pagefaults %ld, Physical pagefaults %ld, Swaps %ld\n\
Blocks in %ld out %ld, Messages in %ld out %ld, Signals %ld\n\
Voluntary context switches %ld, Involuntary context switches %ld\n",
	      (rus.ru_utime.tv_sec * SCALE_SEC +
	       rus.ru_utime.tv_usec / SCALE_USEC) / 100.0,
	      (rus.ru_stime.tv_sec * SCALE_SEC +
	       rus.ru_stime.tv_usec / SCALE_USEC) / 100.0,
	      rus.ru_maxrss, rus.ru_idrss,
	      rus.ru_minflt, rus.ru_majflt,
	      rus.ru_nswap, rus.ru_inblock, rus.ru_oublock,
	      rus.ru_msgsnd, rus.ru_msgrcv, rus.ru_nsignals,
	      rus.ru_nvcsw, rus.ru_nivcsw);
#endif
#if defined(_MSC_VER)
   _CrtSetReportMode( _CRT_WARN, _CRTDBG_MODE_FILE );
   _CrtSetReportFile( _CRT_WARN, _CRTDBG_FILE_STDERR );
   _CrtSetReportMode( _CRT_ERROR, _CRTDBG_MODE_FILE );
   _CrtSetReportFile( _CRT_ERROR, _CRTDBG_FILE_STDERR );
   _CrtSetReportMode( _CRT_ASSERT, _CRTDBG_MODE_FILE );
   _CrtSetReportFile( _CRT_ASSERT, _CRTDBG_FILE_STDERR );
   _CrtCheckMemory();
#endif
  }

  my_thread_end();
  my_thread_global_end();

  if (!(infoflag & MY_DONT_FREE_DBUG))
    DBUG_END();                /* Must be done as late as possible */

  my_mutex_end();
#if defined(SAFE_MUTEX)
  /*
    Check on destroying of mutexes. A few may be left that will get cleaned
    up by C++ destructors
  */
  safe_mutex_end((infoflag & (MY_GIVE_INFO | MY_CHECK_ERROR)) ? stderr :
                 (FILE *) 0);
#endif /* defined(SAFE_MUTEX) */

#ifdef _WIN32
   WSACleanup();
#endif
 
  /* At very last, delete mysys key, it is used everywhere including DBUG */
  pthread_key_delete(THR_KEY_mysys);
  my_init_done= my_thr_key_mysys_exists= 0;
} /* my_end */

#ifdef DBUG_ASSERT_EXISTS
/* Dummy tag function for debugging */

void my_debug_put_break_here(void)
{
}
#endif

#ifdef _WIN32


/*
  my_parameter_handler
  
  Invalid parameter handler we will use instead of the one "baked"
  into the CRT.
*/

void my_parameter_handler(const wchar_t * expression, const wchar_t * function,
                          const wchar_t * file, unsigned int line,
                          uintptr_t pReserved)
{
  __debugbreak();
}


#ifdef __MSVC_RUNTIME_CHECKS
#include <rtcapi.h>

/* Turn off runtime checks for 'handle_rtc_failure' */
#pragma runtime_checks("", off)

/*
  handle_rtc_failure
  Catch the RTC error and dump it to stderr
*/

int handle_rtc_failure(int err_type, const char *file, int line,
                       const char* module, const char *format, ...)
{
  va_list args;
  va_start(args, format);
  fprintf(stderr, "Error:");
  vfprintf(stderr, format, args);
  fprintf(stderr, " At %s:%d\n", file, line);
  va_end(args);
  (void) fflush(stderr);
  __debugbreak();

  return 0; /* Error is handled */
}
#pragma runtime_checks("", restore)
#endif


static void my_win_init(void)
{
  DBUG_ENTER("my_win_init");

#if defined(_MSC_VER)
  _set_invalid_parameter_handler(my_parameter_handler);
#endif

#ifdef __MSVC_RUNTIME_CHECKS
  /*
    Install handler to send RTC (Runtime Error Check) warnings
    to log file
  */
  _RTC_SetErrorFunc(handle_rtc_failure);
#endif

  _tzset();

  /*
   We do not want text translation (LF->CRLF)
   when stdout is console/terminal, it is buggy
  */
  if (fileno(stdout) >= 0 && isatty(fileno(stdout)))
    (void)setmode(fileno(stdout), O_BINARY);

  if (fileno(stderr) >= 0 && isatty(fileno(stderr)))
    (void) setmode(fileno(stderr), O_BINARY);

  setup_codepages();
  DBUG_VOID_RETURN;
}


static my_bool win32_init_tcp_ip()
{
  WORD wVersionRequested = MAKEWORD( 2, 2 );
  WSADATA wsaData;
  if (WSAStartup(wVersionRequested, &wsaData))
  {
    fprintf(stderr, "WSAStartup() failed with error: %d\n", WSAGetLastError());
    return 1;
  }
  return(0);
}
#endif /* _WIN32 */

PSI_stage_info stage_waiting_for_table_level_lock=
{0, "Waiting for table level lock", 0};

#ifdef HAVE_PSI_INTERFACE
#if !defined(HAVE_PREAD) && !defined(_WIN32)
PSI_mutex_key key_my_file_info_mutex;
#endif /* !defined(HAVE_PREAD) && !defined(_WIN32) */

#if !defined(HAVE_LOCALTIME_R) || !defined(HAVE_GMTIME_R)
PSI_mutex_key key_LOCK_localtime_r;
#endif /* !defined(HAVE_LOCALTIME_R) || !defined(HAVE_GMTIME_R) */

PSI_mutex_key key_BITMAP_mutex, key_IO_CACHE_append_buffer_lock,
  key_IO_CACHE_SHARE_mutex, key_KEY_CACHE_cache_lock,
  key_LOCK_alarm, key_LOCK_timer,
  key_my_thread_var_mutex, key_THR_LOCK_charset, key_THR_LOCK_heap,
  key_THR_LOCK_lock, key_THR_LOCK_malloc,
  key_THR_LOCK_mutex, key_THR_LOCK_myisam, key_THR_LOCK_net,
  key_THR_LOCK_open, key_THR_LOCK_threads,
  key_TMPDIR_mutex, key_THR_LOCK_myisam_mmap, key_LOCK_uuid_generator;

static PSI_mutex_info all_mysys_mutexes[]=
{
#if !defined(HAVE_PREAD) && !defined(_WIN32)
  { &key_my_file_info_mutex, "st_my_file_info:mutex", 0},
#endif /* !defined(HAVE_PREAD) && !defined(_WIN32) */
#if !defined(HAVE_LOCALTIME_R) || !defined(HAVE_GMTIME_R)
  { &key_LOCK_localtime_r, "LOCK_localtime_r", PSI_FLAG_GLOBAL},
#endif /* !defined(HAVE_LOCALTIME_R) || !defined(HAVE_GMTIME_R) */
  { &key_BITMAP_mutex, "BITMAP::mutex", 0},
  { &key_IO_CACHE_append_buffer_lock, "IO_CACHE::append_buffer_lock", 0},
  { &key_IO_CACHE_SHARE_mutex, "IO_CACHE::SHARE_mutex", 0},
  { &key_KEY_CACHE_cache_lock, "KEY_CACHE::cache_lock", 0},
  { &key_LOCK_alarm, "LOCK_alarm", PSI_FLAG_GLOBAL},
  { &key_LOCK_timer, "LOCK_timer", PSI_FLAG_GLOBAL},
  { &key_my_thread_var_mutex, "my_thread_var::mutex", 0},
  { &key_THR_LOCK_charset, "THR_LOCK_charset", PSI_FLAG_GLOBAL},
  { &key_THR_LOCK_heap, "THR_LOCK_heap", PSI_FLAG_GLOBAL},
  { &key_THR_LOCK_lock, "THR_LOCK_lock", PSI_FLAG_GLOBAL},
  { &key_THR_LOCK_malloc, "THR_LOCK_malloc", PSI_FLAG_GLOBAL},
  { &key_THR_LOCK_mutex, "THR_LOCK::mutex", 0},
  { &key_THR_LOCK_myisam, "THR_LOCK_myisam", PSI_FLAG_GLOBAL},
  { &key_THR_LOCK_net, "THR_LOCK_net", PSI_FLAG_GLOBAL},
  { &key_THR_LOCK_open, "THR_LOCK_open", PSI_FLAG_GLOBAL},
  { &key_THR_LOCK_threads, "THR_LOCK_threads", PSI_FLAG_GLOBAL},
  { &key_TMPDIR_mutex, "TMPDIR_mutex", PSI_FLAG_GLOBAL},
  { &key_THR_LOCK_myisam_mmap, "THR_LOCK_myisam_mmap", PSI_FLAG_GLOBAL},
  { &key_LOCK_uuid_generator, "LOCK_uuid_generator", PSI_FLAG_GLOBAL }
};

PSI_cond_key key_COND_alarm, key_COND_timer, key_IO_CACHE_SHARE_cond,
  key_IO_CACHE_SHARE_cond_writer, key_my_thread_var_suspend,
  key_THR_COND_threads, key_WT_RESOURCE_cond;

static PSI_cond_info all_mysys_conds[]=
{
  { &key_COND_alarm, "COND_alarm", PSI_FLAG_GLOBAL},
  { &key_COND_timer, "COND_timer", PSI_FLAG_GLOBAL},
  { &key_IO_CACHE_SHARE_cond, "IO_CACHE_SHARE::cond", 0},
  { &key_IO_CACHE_SHARE_cond_writer, "IO_CACHE_SHARE::cond_writer", 0},
  { &key_my_thread_var_suspend, "my_thread_var::suspend", 0},
  { &key_THR_COND_threads, "THR_COND_threads", PSI_FLAG_GLOBAL},
  { &key_WT_RESOURCE_cond, "WT_RESOURCE::cond", 0}
};

PSI_rwlock_key key_SAFEHASH_mutex;

static PSI_rwlock_info all_mysys_rwlocks[]=
{
  { &key_SAFEHASH_mutex, "SAFE_HASH::mutex", 0}
};

#ifdef USE_ALARM_THREAD
PSI_thread_key key_thread_alarm;
#endif
PSI_thread_key key_thread_timer;

static PSI_thread_info all_mysys_threads[]=
{
#ifdef USE_ALARM_THREAD
  { &key_thread_alarm, "alarm", PSI_FLAG_GLOBAL},
#endif
  { &key_thread_timer, "statement_timer", PSI_FLAG_GLOBAL}
};


PSI_file_key key_file_charset, key_file_cnf;

static PSI_file_info all_mysys_files[]=
{
  { &key_file_charset, "charset", 0},
  { &key_file_cnf, "cnf", 0}
};

PSI_stage_info *all_mysys_stages[]=
{
  & stage_waiting_for_table_level_lock
};

void my_init_mysys_psi_keys()
{
  const char* category= "mysys";
  int count;

  count= sizeof(all_mysys_mutexes)/sizeof(all_mysys_mutexes[0]);
  mysql_mutex_register(category, all_mysys_mutexes, count);

  count= sizeof(all_mysys_conds)/sizeof(all_mysys_conds[0]);
  mysql_cond_register(category, all_mysys_conds, count);

  count= sizeof(all_mysys_rwlocks)/sizeof(all_mysys_rwlocks[0]);
  mysql_rwlock_register(category, all_mysys_rwlocks, count);

  count= sizeof(all_mysys_threads)/sizeof(all_mysys_threads[0]);
  mysql_thread_register(category, all_mysys_threads, count);

  count= sizeof(all_mysys_files)/sizeof(all_mysys_files[0]);
  mysql_file_register(category, all_mysys_files, count);

  count= array_elements(all_mysys_stages);
  mysql_stage_register(category, all_mysys_stages, count);
}
#endif /* HAVE_PSI_INTERFACE */

