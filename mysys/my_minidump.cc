/* Copyright (c) 2021, MariaDB Corporation

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

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <my_minidump.h>

#define VERBOSE(fmt,...) \
  if (verbose)  { fprintf(stderr, "my_create_minidump : " fmt,__VA_ARGS__); }

extern "C" BOOL my_create_minidump(DWORD pid, BOOL verbose)
{
  HANDLE file = 0;
  HANDLE process= 0;
  DWORD size= MAX_PATH;
  char path[MAX_PATH];
  char working_dir[MAX_PATH];
  char tmpname[MAX_PATH];
  char *filename= 0;
  bool ret= FALSE;
  process= OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!process)
  {
    VERBOSE("cannot open process pid=%lu to create dump, last error %lu\n",
      pid, GetLastError());
    goto exit;
  }

  if (QueryFullProcessImageName(process, 0, path, &size) == 0)
  {
    VERBOSE("cannot read process path for pid %lu, last error %lu\n",
      pid, GetLastError());
    goto exit;
  }

  filename= strrchr(path, '\\');
  if (filename)
  {
    filename++;
    // We are not interested in dump of some proceses (my_safe_process.exe,cmd.exe)
    // since they are only used to start up other programs.
    // We're interested however in their children;
    const char *exclude_programs[] = {"my_safe_process.exe","cmd.exe", 0};
    for(size_t i=0; exclude_programs[i]; i++)
      if (_stricmp(filename, exclude_programs[i]) == 0)
        goto exit;
  }
  else
    filename= path;

  // Add .dmp extension
  char *p;
  if ((p= strrchr(filename, '.')) == 0)
    p= filename + strlen(filename);

  strncpy(p, ".dmp", path + MAX_PATH - p);

  // Íf file with this name exist, generate unique name with .dmp extension
  if (GetFileAttributes(filename) != INVALID_FILE_ATTRIBUTES)
  {
    if (!GetTempFileName(".", filename, 0, tmpname))
    {
      fprintf(stderr, "GetTempFileName failed, last error %lu", GetLastError());
      goto exit;
    }
    strncat_s(tmpname, ".dmp", sizeof(tmpname));
    filename= tmpname;
  }

  if (!GetCurrentDirectory(MAX_PATH, working_dir))
  {
    VERBOSE("GetCurrentDirectory failed, last error %lu", GetLastError());
    goto exit;
  }

  file= CreateFile(filename, GENERIC_READ | GENERIC_WRITE,
    0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);

  if (file == INVALID_HANDLE_VALUE)
  {
    VERBOSE("CreateFile() failed for file %s, working dir %s, last error = %lu\n",
      filename, working_dir, GetLastError());
    goto exit;
  }

  if (!MiniDumpWriteDump(process, pid, file, MiniDumpNormal, 0, 0, 0))
  {
    VERBOSE("Failed to write minidump to %s, working dir %s, last error %lu\n",
      filename, working_dir, GetLastError());
    goto exit;
  }

  VERBOSE("Minidump written to %s, directory %s\n", filename, working_dir);
  ret= TRUE;
exit:
  if (process != 0 && process != INVALID_HANDLE_VALUE)
    CloseHandle(process);

  if (file != 0 && file != INVALID_HANDLE_VALUE)
    CloseHandle(file);
  return ret;
}
