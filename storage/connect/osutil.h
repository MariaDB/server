#ifndef __OSUTIL_H__
#define __OSUTIL_H__

#if defined(UNIX) || defined(UNIV_LINUX)
#include "my_global.h"
#include <errno.h>
#include <stddef.h>

typedef const void *LPCVOID;
typedef const char *LPCTSTR;
typedef const char *LPCSTR;
typedef unsigned char BYTE;
typedef char *LPSTR;
typedef char *LPTSTR;
typedef char *PSZ;
typedef int   INT;
//typedef int   DWORD;
#undef  HANDLE
typedef int   HANDLE;
#ifdef __cplusplus
//typedef int   bool;
#else
#define bool  my_bool
#endif
#define _MAX_PATH  PATH_MAX
#define stricmp    strcasecmp
#define strnicmp   strncasecmp
#define MB_OK  0x00000000

#if defined(__cplusplus)
#if !defined(__MINMAX_DEFINED)
#define __MINMAX_DEFINED
#ifndef max
#define max(x,y)   (((x)>(y))?(x):(y))
#endif
#ifndef min
#define min(x,y)   (((x)<(y))?(x):(y))
#endif
#endif
#endif  /* __cplusplus */

#ifdef __cplusplus
extern "C" {
#endif

int   GetLastError();
void  _splitpath(const char*, char*, char*, char*, char*);
void  _makepath(char*, const char*, const char*, const char*, const char*);
char *_fullpath(char *absPath, const char *relPath, size_t maxLength);
bool  MessageBeep(uint);
unsigned long _filelength(int fd);

int GetPrivateProfileString(
  LPCTSTR lpAppName,        // section name
  LPCTSTR lpKeyName,        // key name
  LPCTSTR lpDefault,        // default string
  LPTSTR lpReturnedString,  // destination buffer
  int nSize,                // size of destination buffer
  LPCTSTR lpFileName        // initialization file name
  );

uint GetPrivateProfileInt(
  LPCTSTR lpAppName,        // section name
  LPCTSTR lpKeyName,        // key name
  INT nDefault,             // return value if key name not found
  LPCTSTR lpFileName        // initialization file name
  );

bool WritePrivateProfileString(
  LPCTSTR lpAppName,        // section name
  LPCTSTR lpKeyName,        // key name
  LPCTSTR lpString,         // string to add
  LPCTSTR lpFileName        // initialization file
  );

int GetPrivateProfileSection(
  LPCTSTR lpAppName,        // section name
  LPTSTR lpReturnedString,  // return buffer
  int nSize,                // size of return buffer
  LPCTSTR lpFileName        // initialization file name
  );

bool WritePrivateProfileSection(
  LPCTSTR lpAppName,        // section name
  LPCTSTR lpString,         // data
  LPCTSTR lpFileName        // file name
  );

PSZ strupr(PSZ s);
PSZ strlwr(PSZ s);

typedef size_t FILEPOS;
//pedef int    FILEHANDLE; // UNIX

#ifndef _MAX_PATH
#define MAX_PATH 256
#endif

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

#ifdef __cplusplus
}
#endif

#endif /* __OSUTIL_H__ */
