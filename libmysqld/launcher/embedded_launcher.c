/* Copyright (c) 2026, MariaDB plc

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

/*
  MDEV-11111: spawn a private mariadbd and wait for it.

  Lifetime is tied to the client with a "lifeline" pipe: the client holds
  the write end, the server gets the read end (--embedded-lifeline) and
  shuts down gracefully on EOF, i.e. when mariadb_embedded_stop() closes
  the write end, or when the client process dies.

  PR_SET_PDEATHSIG is deliberately not used: on Linux it fires when the
  *thread* that forked exits, which would kill the server whenever the
  application calls mysql_server_init() from a short-lived thread.
*/

#define _GNU_SOURCE /* dladdr */
#include "embedded_launcher.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#ifndef _WIN32
extern char **environ;
#endif

#define DEFAULT_TIMEOUT_SEC 60
#define STOP_TIMEOUT_SEC 60
#define POLL_MS 20

static char error_buf[512];
static char socket_name[256];

#ifdef _WIN32
static HANDLE server_process, lifeline_write;
#else
static pid_t server_pid;
static int lifeline_write= -1;
static char socket_dir[200];
#endif

static int running;

const char *mariadb_embedded_error(void) { return error_buf; }

const char *mariadb_embedded_socket(void)
{
  return running ? socket_name : NULL;
}

static int fail(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(error_buf, sizeof(error_buf), fmt, ap);
  va_end(ap);
  return 1;
}

static int timeout_sec(const char *env, int def)
{
  const char *s= getenv(env);
  int v= s ? atoi(s) : 0;
  return v > 0 ? v : def;
}

/* Server output goes to a file in the private directory, not to the
   application's stderr. On startup failure, show the end of it. */
static char log_path[600];

static void add_log_tail(void)
{
  char tail[400];
  size_t n, len= strlen(error_buf);
  FILE *f= log_path[0] ? fopen(log_path, "r") : NULL;
  if (!f)
    return;
  if (fseek(f, -(long) (sizeof(tail) - 1), SEEK_END))
    rewind(f);
  n= fread(tail, 1, sizeof(tail) - 1, f);
  fclose(f);
  tail[n]= 0;
  snprintf(error_buf + len, sizeof(error_buf) - len, "; server log: %s", tail);
}

/*
  Where is the server: $MARIADB_EMBEDDED_SERVER, else next to this library
  (or executable, if linked statically), else "mariadbd" from PATH.
*/

static const char *server_binary(void)
{
  static char path[1024];
  const char *s= getenv("MARIADB_EMBEDDED_SERVER");
  char *slash;
  if (s && *s)
    return s;
#ifdef _WIN32
  {
    HMODULE h;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR) &server_binary, &h) &&
        GetModuleFileNameA(h, path, sizeof(path) - 16) &&
        (slash= strrchr(path, '\\')))
    {
      strcpy(slash + 1, "mariadbd.exe");
      if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        return path;
    }
  }
#else
  {
    Dl_info info;
    if (dladdr((void *) &server_binary, &info) && info.dli_fname &&
        strlen(info.dli_fname) < sizeof(path) - 16 &&
        (strcpy(path, info.dli_fname), (slash= strrchr(path, '/'))))
    {
      strcpy(slash + 1, "mariadbd");
      if (access(path, X_OK) == 0)
        return path;
    }
  }
#endif
  return "mariadbd";
}

/* Server arguments: fixed embedded ones + whatever the application passed */
static char **build_args(int argc, char **argv, const char *lifeline,
                         int *count)
{
  int i, n= 0;
  char **a= calloc(argc + 8, sizeof(char *));
  char buf[600];
  if (!a)
    return NULL;
  a[n++]= strdup(server_binary());
  /* Application options first: --no-defaults must be the first option, and
     for duplicates the last one wins, so ours below take precedence */
  for (i= 1; i < argc; i++)
    a[n++]= argv[i] ? strdup(argv[i]) : strdup("");
  snprintf(buf, sizeof(buf), "--socket=%s", socket_name);
  a[n++]= strdup(buf);
  a[n++]= strdup("--skip-networking");
#ifdef _WIN32
  a[n++]= strdup("--enable-named-pipe");
#endif
  snprintf(buf, sizeof(buf), "--embedded-lifeline=%s", lifeline);
  a[n++]= strdup(buf);
  a[n]= NULL;
  *count= n;
  return a;
}

static void free_args(char **a, int n)
{
  int i;
  for (i= 0; i < n; i++)
    free(a[i]);
  free(a);
}

#ifdef _WIN32

/* Quote per the CommandLineToArgvW rules */
static void append_quoted(char *dst, size_t size, const char *s)
{
  size_t len= strlen(dst);
  size_t bs= 0;
  if (len + 3 >= size)
    return;
  dst[len++]= '"';
  for (; *s && len + 2 * bs + 4 < size; s++)
  {
    if (*s == '\\')
      bs++;
    else if (*s == '"')
    {
      for (; bs; bs--)
        dst[len++]= '\\';
      dst[len++]= '\\';
    }
    else
      bs= 0;
    dst[len++]= *s;
  }
  for (; bs; bs--)
    dst[len++]= '\\'; /* backslashes before the closing quote */
  dst[len++]= '"';
  dst[len]= 0;
}

int mariadb_embedded_start(int argc, char **argv, char **groups)
{
  HANDLE rd= NULL, wr= NULL, logh= INVALID_HANDLE_VALUE;
  HANDLE nulh= INVALID_HANDLE_VALUE, handles[3];
  DWORD nhandles= 1;
  const char *logenv;
  SECURITY_ATTRIBUTES sa= {sizeof(sa), NULL, TRUE};
  STARTUPINFOEXA si;
  PROCESS_INFORMATION pi;
  SIZE_T attr_size= 0;
  char **args, lifeline[32], pipe_path[300];
  char *cmd;
  size_t cmd_size;
  int nargs, i, timeout, rc= 1;
  ULONGLONG deadline;

  (void) groups;
  if (running)
    return fail("embedded server is already started");

  snprintf(socket_name, sizeof(socket_name), "MariaDB-embedded-%lu-%lu",
           (unsigned long) GetCurrentProcessId(),
           (unsigned long) GetTickCount());
  snprintf(pipe_path, sizeof(pipe_path), "\\\\.\\pipe\\%s", socket_name);

  if (!CreatePipe(&rd, &wr, &sa, 0))
    return fail("CreatePipe failed, error %ld", (long) GetLastError());
  /* Only the read end is inherited */
  SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);

  snprintf(lifeline, sizeof(lifeline), "%llu",
           (unsigned long long) (ULONG_PTR) rd);
  if (!(args= build_args(argc, argv, lifeline, &nargs)))
    goto end;

  cmd_size= 1;
  for (i= 0; i < nargs; i++)
    cmd_size+= 2 * strlen(args[i]) + 3;
  if (!(cmd= calloc(cmd_size, 1)))
    goto end_args;
  for (i= 0; i < nargs; i++)
  {
    if (i)
      strcat(cmd, " ");
    append_quoted(cmd, cmd_size, args[i]);
  }

  /* Inherit only the lifeline, not every inheritable handle of the app */
  handles[0]= rd;
  memset(&si, 0, sizeof(si));
  si.StartupInfo.cb= sizeof(si);
  logenv= getenv("MARIADB_EMBEDDED_LOG");
  if (!logenv || strcmp(logenv, "stderr"))
  {
    char tmp[MAX_PATH];
    /* stdin from NUL, stdout and stderr to a log file, so that startup
       errors are not lost; the log tail is added to the error message */
    if (GetTempPathA(sizeof(tmp), tmp))
      snprintf(log_path, sizeof(log_path), "%s%s.log", tmp, socket_name);
    logh= CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    nulh= CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (logh != INVALID_HANDLE_VALUE && nulh != INVALID_HANDLE_VALUE)
    {
      handles[nhandles++]= logh;
      handles[nhandles++]= nulh;
      si.StartupInfo.dwFlags= STARTF_USESTDHANDLES;
      si.StartupInfo.hStdInput= nulh;
      si.StartupInfo.hStdOutput= logh;
      si.StartupInfo.hStdError= logh;
    }
  }
  InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
  si.lpAttributeList= (LPPROC_THREAD_ATTRIBUTE_LIST) malloc(attr_size);
  if (!si.lpAttributeList ||
      !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0,
                                         &attr_size) ||
      !UpdateProcThreadAttribute(si.lpAttributeList, 0,
                                 PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                 nhandles * sizeof(HANDLE), NULL, NULL))
  {
    fail("cannot set up handle inheritance, error %ld",
         (long) GetLastError());
    goto end_cmd;
  }

  if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                      EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                      NULL, NULL, &si.StartupInfo, &pi))
  {
    fail("cannot start '%s', error %ld", args[0], (long) GetLastError());
    goto end_cmd;
  }
  server_process= pi.hProcess;
  if (logh != INVALID_HANDLE_VALUE)
    CloseHandle(logh);
  if (nulh != INVALID_HANDLE_VALUE)
    CloseHandle(nulh);
  logh= nulh= INVALID_HANDLE_VALUE;

  /*
    No KILL_ON_JOB_CLOSE job object: it would hard-kill the server the moment
    the application dies, racing with (and beating) the graceful shutdown
    that the lifeline pipe triggers.
  */
  CloseHandle(pi.hThread);

  CloseHandle(rd);
  rd= NULL;
  lifeline_write= wr;
  wr= NULL;

  timeout= timeout_sec("MARIADB_EMBEDDED_TIMEOUT", DEFAULT_TIMEOUT_SEC);
  deadline= GetTickCount64() + (ULONGLONG) timeout * 1000;
  for (;;)
  {
    DWORD code;
    if (WaitForSingleObject(server_process, 0) == WAIT_OBJECT_0)
    {
      GetExitCodeProcess(server_process, &code);
      fail("embedded server exited during startup, status %ld",
           (long) code);
      add_log_tail();
      break;
    }
    if (WaitNamedPipeA(pipe_path, POLL_MS))
    {
      _putenv_s("MARIADB_UNIX_PORT", socket_name);
      running= 1;
      rc= 0;
      break;
    }
    if (GetTickCount64() > deadline)
    {
      fail("embedded server did not start within %d seconds", timeout);
      add_log_tail();
      break;
    }
    Sleep(POLL_MS);
  }
  if (!running)
    mariadb_embedded_stop();

end_cmd:
  free(cmd);
  if (si.lpAttributeList)
  {
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
  }
end_args:
  free_args(args, nargs);
end:
  if (rd)
    CloseHandle(rd);
  if (logh != INVALID_HANDLE_VALUE)
    CloseHandle(logh);
  if (nulh != INVALID_HANDLE_VALUE)
    CloseHandle(nulh);
  if (wr)
    CloseHandle(wr);
  return rc;
}

void mariadb_embedded_stop(void)
{
  if (lifeline_write)
  {
    CloseHandle(lifeline_write); /* EOF: the server shuts down gracefully */
    lifeline_write= NULL;
  }
  if (server_process)
  {
    if (WaitForSingleObject(server_process,
                            STOP_TIMEOUT_SEC * 1000) != WAIT_OBJECT_0)
      TerminateProcess(server_process, 1);
    CloseHandle(server_process);
    server_process= NULL;
  }
  if (log_path[0])
  {
    DeleteFileA(log_path);
    log_path[0]= 0;
  }
  running= 0;
}

#else /* !_WIN32 */

static void sleep_ms(int ms)
{
  struct timespec ts= {0, ms * 1000000L};
  nanosleep(&ts, NULL);
}

static int socket_ready(void)
{
  struct sockaddr_un sa;
  int fd, ok;
  memset(&sa, 0, sizeof(sa));
  sa.sun_family= AF_UNIX;
  strncpy(sa.sun_path, socket_name, sizeof(sa.sun_path) - 1);
  if ((fd= socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
    return 0;
  ok= connect(fd, (struct sockaddr *) &sa, sizeof(sa)) == 0;
  close(fd);
  return ok;
}

int mariadb_embedded_start(int argc, char **argv, char **groups)
{
  int fds[2], nargs, timeout, rc= 1, waited_ms= 0;
  char **args, lifeline[16];
  const char *tmp;
  struct sockaddr_un sa;

  (void) groups;
  if (running)
    return fail("embedded server is already started");

  /* Private directory: only the current user can reach the socket */
  tmp= getenv("TMPDIR");
  snprintf(socket_dir, sizeof(socket_dir), "%s/mariadb-embedded-XXXXXX",
           tmp && *tmp ? tmp : "/tmp");
  if (!mkdtemp(socket_dir))
    return fail("cannot create temporary directory: %s", strerror(errno));
  snprintf(socket_name, sizeof(socket_name), "%s/mysqld.sock", socket_dir);
  if (strlen(socket_name) >= sizeof(sa.sun_path))
  {
    rmdir(socket_dir);
    return fail("socket path is too long: %s", socket_name);
  }

  if (pipe(fds))
  {
    rmdir(socket_dir);
    return fail("pipe() failed: %s", strerror(errno));
  }
  fcntl(fds[1], F_SETFD, FD_CLOEXEC); /* write end stays with us */

  snprintf(lifeline, sizeof(lifeline), "%d", fds[0]);
  if (!(args= build_args(argc, argv, lifeline, &nargs)))
    goto err;

  /* posix_spawn, not fork+exec: safe to call from a multithreaded host */
  {
    pid_t pid;
    posix_spawn_file_actions_t fa;
    const char *log= getenv("MARIADB_EMBEDDED_LOG");
    int sp;
    posix_spawn_file_actions_init(&fa);
    if (!log || strcmp(log, "stderr"))
    {
      snprintf(log_path, sizeof(log_path), "%s/mysqld.log", socket_dir);
      posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
      posix_spawn_file_actions_addopen(&fa, 2, log_path,
                                       O_WRONLY | O_CREAT | O_TRUNC, 0600);
    }
    sp= posix_spawnp(&pid, args[0], &fa, NULL, args, environ);
    posix_spawn_file_actions_destroy(&fa);
    free_args(args, nargs);
    if (sp)
    {
      fail("cannot execute embedded server '%s': %s", server_binary(),
           strerror(sp));
      goto err;
    }
    server_pid= pid;
  }
  close(fds[0]);
  lifeline_write= fds[1];

  timeout= timeout_sec("MARIADB_EMBEDDED_TIMEOUT", DEFAULT_TIMEOUT_SEC);
  for (;;)
  {
    int status;
    pid_t r= waitpid(server_pid, &status, WNOHANG);
    if (r == server_pid)
    {
      server_pid= 0;
      fail("embedded server exited during startup, status %ld",
             (long) (WIFEXITED(status) ? WEXITSTATUS(status)
                                        : 128 + WTERMSIG(status)));
      add_log_tail();
      break;
    }
    if (socket_ready())
    {
      setenv("MARIADB_UNIX_PORT", socket_name, 1);
      running= 1;
      rc= 0;
      break;
    }
    if (waited_ms >= timeout * 1000)
    {
      fail("embedded server did not start within %d seconds", timeout);
      add_log_tail();
      break;
    }
    sleep_ms(POLL_MS);
    waited_ms+= POLL_MS;
  }
  if (rc)
    mariadb_embedded_stop();
  return rc;

err:
  close(fds[0]);
  close(fds[1]);
  if (log_path[0])
    unlink(log_path);
  log_path[0]= 0;
  rmdir(socket_dir);
  socket_dir[0]= 0;
  return 1;
}

void mariadb_embedded_stop(void)
{
  if (lifeline_write >= 0)
  {
    close(lifeline_write); /* EOF: the server shuts down gracefully */
    lifeline_write= -1;
  }
  if (server_pid > 0)
  {
    int waited_ms= 0, status;
    while (waitpid(server_pid, &status, WNOHANG) == 0)
    {
      if (waited_ms >= STOP_TIMEOUT_SEC * 1000)
      {
        kill(server_pid, SIGKILL);
        waitpid(server_pid, &status, 0);
        break;
      }
      sleep_ms(POLL_MS);
      waited_ms+= POLL_MS;
    }
    server_pid= 0;
  }
  if (socket_dir[0])
  {
    unlink(socket_name); /* normally removed by the server already */
    if (log_path[0])
      unlink(log_path);
    log_path[0]= 0;
    rmdir(socket_dir);
    socket_dir[0]= 0;
  }
  running= 0;
}

#endif /* _WIN32 */
