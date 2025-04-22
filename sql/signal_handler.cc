/* Copyright (c) 2011, 2012, Oracle and/or its affiliates.
   Copyright (c) 2011, 2021, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#include "mariadb.h"
#include "my_dbug.h"
#include <signal.h>

//#include "sys_vars.h"
#include <keycache.h>
#include "mysqld.h"
#include "sql_class.h"
#include "my_stacktrace.h"
#include <source_revision.h>

#ifdef WITH_WSREP
#include "wsrep_server_state.h"
#endif /* WITH_WSREP */

#ifdef __WIN__
#include <crtdbg.h>
#include <direct.h>
#define SIGNAL_FMT "exception 0x%x"
#else
#define SIGNAL_FMT "signal %d"
#endif


#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/*
  We are handling signals/exceptions in this file.
  Any global variables we read should be 'volatile sig_atomic_t'
  to guarantee that we read some consistent value.
 */
static volatile sig_atomic_t segfaulted= 0;
extern ulong max_used_connections;
extern volatile sig_atomic_t calling_initgroups;

extern const char *optimizer_switch_names[];

static inline void output_core_info()
{
  /* proc is optional on some BSDs so it can't hurt to look */
#if defined(HAVE_READLINK) && !defined(__APPLE__) && !defined(__FreeBSD__)
  char buff[PATH_MAX];
  ssize_t len;
  int fd;
  if ((len= readlink("/proc/self/cwd", buff, sizeof(buff)-1)) >= 0)
  {
    buff[len]= 0;
    my_safe_printf_stderr("Writing a core file...\nWorking directory at %.*s\n",
                          (int) len, buff);
  }
#ifdef __FreeBSD__
  if ((fd= open("/proc/curproc/rlimit", O_RDONLY)) >= 0)
#else
  if ((fd= open("/proc/self/limits", O_RDONLY)) >= 0)
#endif
  {
    char *endline= buff;
    ssize_t remain_len= len= read(fd, buff, sizeof(buff));
    close(fd);
    my_safe_printf_stderr("Resource Limits (excludes unlimited resources):\n");
    /* first line, header */
    endline= (char *) memchr(buff, '\n', remain_len);
    if (endline)
    {
      endline++;
      remain_len= buff + len - endline;
      my_safe_printf_stderr("%.*s", (int) (endline - buff), buff);

      while (remain_len > 27)
      {
        char *newendline= (char *) memchr(endline, '\n', remain_len);
	if (!newendline)
          break;
        *newendline= '\0';
        newendline++;
        if (endline[26] != 'u') /* skip unlimited limits */
          my_safe_printf_stderr("%s\n", endline);

        remain_len-= newendline - endline;
        endline= newendline;
      }
    }
  }
#ifdef __linux__
  if ((fd= open("/proc/sys/kernel/core_pattern", O_RDONLY)) >= 0)
  {
    len= read(fd, (uchar*)buff, sizeof(buff));
    my_safe_printf_stderr("Core pattern: %.*s\n", (int) len, buff);
    close(fd);
  }
  if ((fd= open("/proc/version", O_RDONLY)) >= 0)
  {
    len= read(fd, (uchar*)buff, sizeof(buff));
    my_safe_printf_stderr("Kernel version: %.*s\n", (int) len, buff);
    close(fd);
  }
#endif
#elif defined(__APPLE__) || defined(__FreeBSD__)
  char buff[PATH_MAX];
  size_t len = sizeof(buff);
  if (sysctlbyname("kern.corefile", buff, &len, NULL, 0) == 0)
  {
    my_safe_printf_stderr("Core pattern: %.*s\n", (int) len, buff);
  }
  if (sysctlbyname("kern.version", buff, &len, NULL, 0) == 0)
  {
    my_safe_printf_stderr("Kernel version: %.*s\n", (int) len, buff);
  }
#elif defined(HAVE_GETCWD)
  char buff[80];

  if (getcwd(buff, sizeof(buff)))
  {
    my_safe_printf_stderr("Writing a core file at %.*s\n", (int) sizeof(buff), buff);
    fflush(stderr);
  }
#endif
}

/**
 * Handler for fatal signals on POSIX, exception handler on Windows.
 *
 * Fatal events (seg.fault, bus error etc.) will trigger
 * this signal handler.  The handler will try to dump relevant
 * debugging information to stderr and dump a core image.
 *
 * POSIX : Signal handlers should, if possible, only use a set of 'safe' system 
 * calls and library functions.  A list of safe calls in POSIX systems
 * are available at:
 *  http://pubs.opengroup.org/onlinepubs/009695399/functions/xsh_chap02_04.html
 *
 * @param sig Signal number /Exception code
*/
extern "C" sig_handler handle_fatal_signal(int sig)
{
  time_t curr_time;
  struct tm tm;
#ifdef HAVE_STACKTRACE
  THD *thd;
  /*
     This flag remembers if the query pointer was found invalid.
     We will try and print the query at the end of the signal handler, in case
     we're wrong.
  */
#endif

  if (segfaulted)
  {
    my_safe_printf_stderr("Fatal " SIGNAL_FMT " while backtracing\n", sig);
    goto end;
  }
  segfaulted = 1;
  DBUG_PRINT("error", ("handling fatal signal"));

  curr_time= my_time(0);
  localtime_r(&curr_time, &tm);

  my_safe_printf_stderr("%02d%02d%02d %2d:%02d:%02d ",
                        tm.tm_year % 100, tm.tm_mon+1, tm.tm_mday,
                        tm.tm_hour, tm.tm_min, tm.tm_sec);
  if (opt_expect_abort
#ifdef _WIN32
    && sig == (int)EXCEPTION_BREAKPOINT /* __debugbreak in my_sigabrt_hander() */
#else
    && sig == SIGABRT
#endif
    )
  {
    fprintf(stderr,"[Note] mysqld did an expected abort\n");
    goto end;
  }

  my_safe_printf_stderr("[ERROR] %s got " SIGNAL_FMT " ;\n", my_progname, sig);

  my_safe_printf_stderr("%s",
                        "Sorry, we probably made a mistake, and this is a bug.\n\n"
                        "Your assistance in bug reporting will enable us to fix this for the next release.\n"
                        "To report this bug, see https://mariadb.com/kb/en/reporting-bugs about how to report\n"
                        "a bug on https://jira.mariadb.org/.\n\n"
                        "Please include the information from the server start above, to the end of the\n"
                        "information below.\n\n");

  set_server_version(server_version, sizeof(server_version));
  my_safe_printf_stderr("Server version: %s source revision: %s\n\n",
                        server_version, SOURCE_REVISION);

#ifdef WITH_WSREP
  Wsrep_server_state::handle_fatal_signal();
#endif /* WITH_WSREP */

#ifdef HAVE_STACKTRACE
  thd= current_thd;

  if (opt_stack_trace)
  {
    my_safe_printf_stderr("%s",
      "The information page at "
      "https://mariadb.com/kb/en/how-to-produce-a-full-stack-trace-for-mariadbd/\n"
      "contains instructions to obtain a better version of the backtrace below.\n"
      "Following these instructions will help MariaDB developers provide a fix quicker.\n\n"
      "Attempting backtrace. Include this in the bug report.\n"
      "(note: Retrieving this information may fail)\n\n");
    my_safe_printf_stderr("Thread pointer: %p\n", thd);
    my_print_stacktrace(thd ? (uchar*) thd->thread_stack : NULL,
                        (ulong)my_thread_stack_size, 0);
  }
  if (thd)
  {
    const char *kreason= "UNKNOWN";
    switch (thd->killed) {
    case NOT_KILLED:
    case KILL_HARD_BIT:
      kreason= "NOT_KILLED";
      break;
    case KILL_BAD_DATA:
    case KILL_BAD_DATA_HARD:
      kreason= "KILL_BAD_DATA";
      break;
    case KILL_CONNECTION:
    case KILL_CONNECTION_HARD:
      kreason= "KILL_CONNECTION";
      break;
    case KILL_QUERY:
    case KILL_QUERY_HARD:
      kreason= "KILL_QUERY";
      break;
    case KILL_TIMEOUT:
    case KILL_TIMEOUT_HARD:
      kreason= "KILL_TIMEOUT";
      break;
    case KILL_SYSTEM_THREAD:
    case KILL_SYSTEM_THREAD_HARD:
      kreason= "KILL_SYSTEM_THREAD";
      break;
    case KILL_SERVER:
    case KILL_SERVER_HARD:
      kreason= "KILL_SERVER";
      break;
    case ABORT_QUERY:
    case ABORT_QUERY_HARD:
      kreason= "ABORT_QUERY";
      break;
    case KILL_SLAVE_SAME_ID:
      kreason= "KILL_SLAVE_SAME_ID";
      break;
    case KILL_WAIT_TIMEOUT:
    case KILL_WAIT_TIMEOUT_HARD:
      kreason= "KILL_WAIT_TIMEOUT";
      break;
    }

    my_safe_printf_stderr("\nConnection ID (thread ID): %lu\n",
                          (ulong) thd->thread_id);
    my_safe_printf_stderr("Status: %s\n", kreason);
    my_safe_printf_stderr("Query (%p): ", thd->query());
    my_safe_print_str(thd->query(), MY_MIN(65536U, thd->query_length()));
    my_safe_printf_stderr("%s", "Optimizer switch: ");
    ulonglong optsw= thd->variables.optimizer_switch;
    for (uint i= 0; optimizer_switch_names[i+1]; i++, optsw >>= 1)
    {
      if (i)
        my_safe_printf_stderr("%s", ",");
      my_safe_printf_stderr("%s=%s",
              optimizer_switch_names[i], optsw & 1 ? "on" : "off");
    }
    my_safe_printf_stderr("%s", "\n\n");
  }

#endif /* HAVE_STACKTRACE */

  output_core_info();
#ifdef HAVE_WRITE_CORE
  if (test_flags & TEST_CORE_ON_SIGNAL)
  {
    my_write_core(sig);
  }
#endif

end:
#ifndef __WIN__
  /*
     Quit, without running destructors (etc.)
     Use a signal, because the parent (systemd) can check that with WIFSIGNALED
     On Windows, do not terminate, but pass control to exception filter.
  */
  signal(sig, SIG_DFL);
  kill(getpid(), sig);
#else
  return;
#endif
}
