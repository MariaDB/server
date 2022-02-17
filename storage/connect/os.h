/* Copyright (C) MariaDB Corporation Ab */
#ifndef _OS_H_INCLUDED
#define _OS_H_INCLUDED

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__) || defined(__APPLE__)
typedef off_t off64_t;
#define lseek64(fd, offset, whence) lseek((fd), (offset), (whence))
#define open64(path, flags, mode)   open((path), (flags), (mode))
#define ftruncate64(fd, length)     ftruncate((fd), (length))
#define O_LARGEFILE 0
#endif

#ifdef _AIX
#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#endif

#if defined(_WIN32)
typedef __int64 BIGINT;
typedef _Null_terminated_ const char *PCSZ;
#else   // !_WIN32
typedef longlong  BIGINT;
#define FILE_BEGIN    SEEK_SET  
#define FILE_CURRENT  SEEK_CUR  
#define FILE_END      SEEK_END  
typedef const char *PCSZ;
#endif  // !_WIN32


#if !defined(_WIN32)
typedef const void *LPCVOID;
typedef const char *LPCTSTR;
typedef const char *LPCSTR;
typedef unsigned char BYTE;
typedef char *LPSTR;
typedef char *LPTSTR;
typedef char *PSZ;
typedef long BOOL;
typedef int INT;
#if !defined(NODW)
/*
  sqltypes.h from unixODBC incorrectly defines
  DWORD as "unsigned int" instead of "unsigned long" on 64-bit platforms.
  Add "#define NODW" into all files including this file that include
  sqltypes.h (through sql.h or sqlext.h).
*/
typedef unsigned long DWORD;
#endif   // !NODW
#undef  HANDLE     
typedef int   HANDLE;

#define stricmp     strcasecmp
#define _stricmp    strcasecmp
#define strnicmp    strncasecmp
#define _strnicmp   strncasecmp
#ifdef PATH_MAX
#define _MAX_PATH   PATH_MAX
#else
#define _MAX_PATH   FN_REFLEN
#endif
#define _MAX_DRIVE    3
#define _MAX_DIR    FN_REFLEN
#define _MAX_FNAME  FN_HEADLEN
#define _MAX_EXT    FN_EXTLEN
#define INVALID_HANDLE_VALUE  (-1)
#define __stdcall
#endif /* !_WIN32 */

#endif /* _OS_H_INCLUDED */
