/* Copyright (C) MariaDB Corporation Ab */
#ifndef __OSUTIL_H__
#define __OSUTIL_H__

#if defined(UNIX) || defined(UNIV_LINUX)
#if defined(MARIADB)
#include "my_global.h"
#else
#include "mini-global.h"
#endif
#include <errno.h>
#include <stddef.h>
#include "os.h"

#define MB_OK  0x00000000

#ifdef __cplusplus
extern "C" {
#endif

int   GetLastError();
void  _splitpath(const char*, char*, char*, char*, char*);
void  _makepath(char*, const char*, const char*, const char*, const char*);
char *_fullpath(char *absPath, const char *relPath, size_t maxLength);
BOOL  MessageBeep(uint);
unsigned long _filelength(int fd);

PSZ strupr(PSZ s);
PSZ strlwr(PSZ s);

typedef size_t FILEPOS;
//pedef int    FILEHANDLE; // UNIX

#ifdef __cplusplus
}
#endif

#else /* WINDOWS */
#include <windows.h>

typedef __int64 FILEPOS;
//pedef HANDLE  FILEHANDLE; // Win32

#endif /* WINDOWS */

#define XSTR(x) ((x)?(x):"<null>")


#ifdef __cplusplus
extern "C" {
#endif
my_bool CloseFileHandle(HANDLE h);
#ifdef __cplusplus
}
#endif


#endif /* __OSUTIL_H__ */
