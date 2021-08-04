#include "my_global.h"
#ifdef UNIX
#include "osutil.h"
#include <errno.h>
#include <stddef.h>
#else /* WINDOWS */
//#include <windows.h>
#include "osutil.h"
#endif /* WINDOWS */
#include <stdlib.h>
#include <stdio.h>

#include "global.h"
#include "plgdbsem.h"
#include "maputil.h"

#ifdef _WIN32
/***********************************************************************/
/*  In Insert mode, just open the file for append. Otherwise           */
/*  create the mapping file object. The map handle can be released     */
/*  immediately because they will not be used anymore.                 */
/*  If del is true in DELETE mode, then delete the whole file.         */
/*  Returns the file handle that can be used by caller.                */
/***********************************************************************/
HANDLE CreateFileMap(PGLOBAL g, LPCSTR filename,
                     MEMMAP *mm, MODE mode, bool del)
  {
  HANDLE hFile;
  HANDLE hFileMap;
  DWORD  access, share, disposition;

  memset(mm, 0, sizeof(MEMMAP));
  *g->Message = '\0';

  switch (mode) {
    case MODE_READ:
      access = GENERIC_READ;
      share = FILE_SHARE_READ;
      disposition = OPEN_EXISTING;
      break;
    case MODE_UPDATE:
    case MODE_DELETE:
      access = GENERIC_READ | GENERIC_WRITE;
      share = 0;
      disposition = (del) ? TRUNCATE_EXISTING : OPEN_EXISTING;
      break;
    case MODE_INSERT:
      access = GENERIC_WRITE;
      share = 0;
      disposition = OPEN_ALWAYS;
      break;
    default:
      sprintf(g->Message, MSG(BAD_FUNC_MODE), "CreateFileMap", mode);
      return INVALID_HANDLE_VALUE;
    } // endswitch

  hFile = CreateFile(filename, access, share, NULL,  disposition,
                     FILE_ATTRIBUTE_NORMAL, NULL);
  
  if (hFile != INVALID_HANDLE_VALUE)
    if (mode != MODE_INSERT) {
      /*****************************************************************/
      /*  Create the file-mapping object.                              */
      /*****************************************************************/
      access = (mode == MODE_READ) ? PAGE_READONLY : PAGE_READWRITE;
      hFileMap = CreateFileMapping(hFile, NULL,  access, 0, 0, NULL);
      
      if (!hFileMap) {
        DWORD ler = GetLastError();
      
        if (ler && ler != 1006) {
          sprintf(g->Message, MSG(FILE_MAP_ERROR), filename, ler);
          CloseHandle(hFile);
          return INVALID_HANDLE_VALUE;
        } else {
          sprintf(g->Message, MSG(FILE_IS_EMPTY), filename);
          return hFile;
        } // endif ler
      
        } // endif hFileMap
      
      access = (mode == MODE_READ) ? FILE_MAP_READ : FILE_MAP_WRITE;

      if (!(mm->memory = MapViewOfFile(hFileMap, access, 0, 0, 0))) {
        DWORD ler = GetLastError();
      
        sprintf(g->Message, "Error %ld in MapViewOfFile %s", 
                ler, filename);
        CloseHandle(hFile);
        return INVALID_HANDLE_VALUE;
        } // endif memory

      // lenH is the high-order word of the file size
      mm->lenL = GetFileSize(hFile, &mm->lenH);
      CloseHandle(hFileMap);                    // Not used anymore
    }  else // MODE_INSERT
      /*****************************************************************/
      /*  The starting point must be the end of file as for append.    */
      /*****************************************************************/
      SetFilePointer(hFile, 0, NULL, FILE_END);

  return hFile;  
  }  // end of CreateFileMap

bool CloseMemMap(LPVOID memory, size_t dwSize) 
  {
  return (memory) ? !UnmapViewOfFile(memory) : false;
  } // end of CloseMemMap

#else  /* UNIX */
// Code to handle Linux and Solaris
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

/***********************************************************************/
/*  In Insert mode, just open the file for append. Otherwise           */
/*  create the mapping file object. The map handle can be released     */
/*  immediately because they will not be used anymore.                 */
/*  If del is true in DELETE mode, then delete the whole file.         */
/*  Returns the file handle that can be used by caller.                */
/***********************************************************************/
HANDLE CreateFileMap(PGLOBAL g, LPCSTR fileName, 
                         MEMMAP *mm, MODE mode, bool del) 
  {
  unsigned int openMode;
  int          protmode;
  HANDLE       fd;
  size_t       filesize;
  struct stat  st; 

  memset(mm, 0, sizeof(MEMMAP));
  *g->Message = '\0';

  switch (mode) {
    case MODE_READ:
      openMode = O_RDONLY;
      protmode = PROT_READ;
      break;
    case MODE_UPDATE:
    case MODE_DELETE:
      openMode = (del) ? (O_RDWR | O_TRUNC) : O_RDWR;
      protmode = PROT_READ | PROT_WRITE;
      break;
    case MODE_INSERT:
      openMode = (O_WRONLY | O_CREAT | O_APPEND);
      protmode = PROT_WRITE;
      break;
     default:
      sprintf(g->Message, MSG(BAD_FUNC_MODE), "CreateFileMap", mode);
      return INVALID_HANDLE_VALUE;
   } // endswitch

  // Try to open the addressed file.
  fd= global_open(g, MSGID_NONE, fileName, openMode);

  if (fd != INVALID_HANDLE_VALUE && mode != MODE_INSERT) {
    /* We must know about the size of the file. */
    if (fstat(fd, &st)) {
      sprintf(g->Message, MSG(FILE_MAP_ERROR), fileName, errno);
      close(fd);
      return INVALID_HANDLE_VALUE;
      }  // endif fstat
    
    if ((filesize = st.st_size))
      // Now we are ready to load the file.  If mmap() is available we try
      //   this first.  If not available or it failed we try to load it.
      mm->memory = mmap(NULL, filesize, protmode, MAP_SHARED, fd, 0);
    else
      mm->memory = 0;

    if (mm->memory != MAP_FAILED) {
      mm->lenL = (mm->memory != 0) ? filesize : 0;
      mm->lenH = 0;
    } else {
      strcpy(g->Message, "Memory mapping failed");
      close(fd);
      return INVALID_HANDLE_VALUE;
    } // endif memory

    }  /* endif fd */
  
  // mmap() call was successful. ??????????
  return fd;
  }  // end of CreateFileMap

bool CloseMemMap(void *memory, size_t dwSize) 
  {
  if (memory) {
    // All this must be redesigned
    msync((char*)memory, dwSize, MS_SYNC);
    return (munmap((char*)memory, dwSize) < 0) ? true : false;
  } else
    return false;

  }  // end of CloseMemMap

#endif   // UNIX
