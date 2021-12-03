/* 2019, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/*
 Replacement of the buggy implementations of popen in Windows CRT
*/
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <mutex>
#include <stdlib.h>
#include <unordered_map>

enum
{
  REDIRECT_STDIN= 'w',
  REDIRECT_STDOUT= 'r'
};

/** Map from FILE* returned by popen() to corresponding process handle.*/
static std::unordered_map<FILE *, HANDLE> popen_map;
/* Mutex to protect the map.*/
static std::mutex popen_mtx;

/**
Creates a FILE* from HANDLE.
*/
static FILE *make_fp(HANDLE *handle, const char *mode)
{
  int flags = 0;

  if (mode[0] == REDIRECT_STDOUT)
    flags |= O_RDONLY;
  switch (mode[1])
  {
  case 't':
    flags |= _O_TEXT;
    break;
  case 'b':
    flags |= _O_BINARY;
    break;
  }

  int fd= _open_osfhandle((intptr_t) *handle, flags);
  if (fd < 0)
    return NULL;
  FILE *fp= fdopen(fd, mode);
  if (!fp)
  {
    /* Closing file descriptor also closes underlying handle.*/
    close(fd);
    *handle= 0;
  }
  return fp;
}

/** A home-backed version of popen(). */
extern "C" FILE *my_win_popen(const char *cmd, const char *mode)
{
  FILE *fp(0);
  char type= mode[0];
  HANDLE parent_pipe_end(0);
  HANDLE child_pipe_end(0);
  PROCESS_INFORMATION pi{};
  STARTUPINFO si{};
  std::string command_line;

  /* Create a pipe between this and child process.*/
  SECURITY_ATTRIBUTES sa_attr{};
  sa_attr.nLength= sizeof(SECURITY_ATTRIBUTES);
  sa_attr.bInheritHandle= TRUE;
  switch (type)
  {
  case REDIRECT_STDIN:
    if (!CreatePipe(&child_pipe_end, &parent_pipe_end, &sa_attr, 0))
      goto error;
    break;
  case REDIRECT_STDOUT:
    if (!CreatePipe(&parent_pipe_end, &child_pipe_end, &sa_attr, 0))
      goto error;
    break;
  default:
    /* Unknown mode, éxpected "r", "rt", "w", "wt" */
    abort();
  }
  if (!SetHandleInformation(parent_pipe_end, HANDLE_FLAG_INHERIT, 0))
    goto error;

  /* Start child process with redirected output.*/

  si.cb= sizeof(STARTUPINFO);
  si.hStdError= GetStdHandle(STD_ERROR_HANDLE);
  si.hStdOutput= (type == REDIRECT_STDOUT) ? child_pipe_end
                                           : GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdInput= (type == REDIRECT_STDIN) ? child_pipe_end
                                         : GetStdHandle(STD_INPUT_HANDLE);

  si.dwFlags|= STARTF_USESTDHANDLES;
  command_line.append("cmd.exe /c ").append(cmd);

  if (!CreateProcess(0, (LPSTR) command_line.c_str(), 0, 0, TRUE, 0, 0, 0, &si,
                     &pi))
    goto error;

  CloseHandle(pi.hThread);
  CloseHandle(child_pipe_end);
  child_pipe_end= 0;

  fp= make_fp(&parent_pipe_end, mode);
  if (fp)
  {
    std::unique_lock<std::mutex> lk(popen_mtx);
    popen_map[fp]= pi.hProcess;
    return fp;
  }

error:
  for (auto handle : { parent_pipe_end, child_pipe_end })
  {
    if (handle)
      CloseHandle(handle);
  }

  if (pi.hProcess)
  {
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
  }
  return NULL;
}

/** A home-backed version of pclose(). */

extern "C" int my_win_pclose(FILE *fp)
{
  /* Find process entry for given file pointer.*/
  std::unique_lock<std::mutex> lk(popen_mtx);
  HANDLE proc= popen_map[fp];
  if (!proc)
  {
    errno= EINVAL;
    return -1;
  }
  popen_map.erase(fp);
  lk.unlock();

  fclose(fp);

  /* Wait for process to complete, return its exit code.*/
  DWORD ret;
  if (WaitForSingleObject(proc, INFINITE) || !GetExitCodeProcess(proc, &ret))
  {
    ret= -1;
    errno= EINVAL;
  }
  CloseHandle(proc);
  return ret;
}
