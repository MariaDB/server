/* Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.

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
  Utility program used to signal a safe_process it's time to shutdown

  Usage:
    safe_kill <pid>
*/

#include <windows.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <psapi.h>

#ifdef _MSC_VER
/* Silence warning in OS header dbghelp.h */
#pragma warning(push)
#pragma warning(disable : 4091)
#endif

#include <dbghelp.h>

#ifdef _MSC_VER
/* Silence warning in OS header dbghelp.h */
#pragma warning(pop)
#endif

#include <tlhelp32.h>
#include <vector>


static std::vector<DWORD> find_children(DWORD pid)
{
  HANDLE h= NULL;
  PROCESSENTRY32 pe={ 0 };
  std::vector<DWORD> children;

  pe.dwSize = sizeof(PROCESSENTRY32);
  h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if(h == INVALID_HANDLE_VALUE)
    return children;

  for (BOOL ret = Process32First(h, &pe); ret; ret = Process32Next(h, &pe))
  {
    if (pe.th32ParentProcessID == pid)
      children.push_back(pe.th32ProcessID);
  }
  CloseHandle(h);
  return children;
}

void dump_single_process(DWORD pid)
{
  HANDLE file = 0;
  HANDLE process= 0;
  DWORD size= MAX_PATH;
  char path[MAX_PATH];
  char working_dir[MAX_PATH];
  char tmpname[MAX_PATH];

  process= OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!process)
  {
    fprintf(stderr, "safe_kill : cannot open process pid=%u to create dump, last error %u\n",
      pid, GetLastError());
    goto exit;
  }

  if (QueryFullProcessImageName(process, 0, path, &size) == 0)
  {
    fprintf(stderr, "safe_kill : cannot read process path for pid %u, last error %u\n",
      pid, GetLastError());
    goto exit;
  }

  char *filename= strrchr(path, '\\');
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
      fprintf(stderr, "GetTempFileName failed, last error %u", GetLastError());
      goto exit;
    }
    strncat(tmpname, ".dmp", sizeof(tmpname));
    filename= tmpname;
  }

  
  if (!GetCurrentDirectory(MAX_PATH, working_dir))
  {
    fprintf(stderr, "GetCurrentDirectory failed, last error %u", GetLastError());
    goto exit;
  }

  file= CreateFile(filename, GENERIC_READ | GENERIC_WRITE,
    0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);

  if (file == INVALID_HANDLE_VALUE)
  {
    fprintf(stderr, "safe_kill : CreateFile() failed for file %s, working dir %s, last error = %u\n",
      filename, working_dir, GetLastError());
    goto exit;
  }

  if (!MiniDumpWriteDump(process, pid, file, MiniDumpNormal, 0, 0, 0))
  {
    fprintf(stderr, "Failed to write minidump to %s, working dir %s, last error %u\n",
      filename, working_dir, GetLastError());
    goto exit;
  }

  fprintf(stderr, "Minidump written to %s, directory %s\n", filename, working_dir);

exit:
  if (process != 0 && process != INVALID_HANDLE_VALUE)
    CloseHandle(process);

  if (file != 0 && file != INVALID_HANDLE_VALUE)
    CloseHandle(file);
}


static int create_dump(DWORD pid, int recursion_depth= 5)
{
  if (recursion_depth < 0)
    return 0;

  dump_single_process(pid);
  std::vector<DWORD> children= find_children(pid);
  for(size_t i=0; i < children.size(); i++)
    create_dump(children[i], recursion_depth -1);
  return 0;
}


int main(int argc, const char** argv )
{
  DWORD pid= -1;
  HANDLE shutdown_event;
  char safe_process_name[32]= {0};
  int retry_open_event= 2;
  /* Ignore any signals */
  signal(SIGINT,   SIG_IGN);
  signal(SIGBREAK, SIG_IGN);
  signal(SIGTERM,  SIG_IGN);

  if ((argc != 2 && argc != 3) || (argc == 3 && strcmp(argv[2],"dump"))) {
    fprintf(stderr, "safe_kill <pid> [dump]\n");
    exit(2);
  }
  pid= atoi(argv[1]);

  if (argc == 3)
  {
    return create_dump(pid);
  }
  _snprintf(safe_process_name, sizeof(safe_process_name),
            "safe_process[%d]", pid);

  /* Open the event to signal */
  while ((shutdown_event=
          OpenEvent(EVENT_MODIFY_STATE, FALSE, safe_process_name)) == NULL)
  {
     /*
      Check if the process is alive, otherwise there is really
      no sense to retry the open of the event
     */
    HANDLE process;
    DWORD exit_code;
    process= OpenProcess(SYNCHRONIZE| PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!process)
    {
      /* Already died */
      exit(1);
    }

    if (!GetExitCodeProcess(process,&exit_code))
    {
       fprintf(stderr,  "GetExitCodeProcess failed, pid= %d, err= %d\n",
         pid, GetLastError());
       exit(1);
    }

    if (exit_code != STILL_ACTIVE)
    {
       /* Already died */
       CloseHandle(process);
       exit(2);
    }

    CloseHandle(process);

    if (retry_open_event--)
      Sleep(100);
    else
    {
      fprintf(stderr, "Failed to open shutdown_event '%s', error: %d\n",
              safe_process_name, GetLastError());
      exit(3);
    }
  }

  if(SetEvent(shutdown_event) == 0)
  {
    fprintf(stderr, "Failed to signal shutdown_event '%s', error: %d\n",
            safe_process_name, GetLastError());
    CloseHandle(shutdown_event);
    exit(4);
  }
  CloseHandle(shutdown_event);
  exit(0);
}

