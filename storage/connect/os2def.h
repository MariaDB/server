
/****************************** Module Header ******************************/
/* Module Name: OS2DEF.H                                                   */
/* OS/2 Common Definitions file adapted for UNIX system.                   */
/***************************************************************************/
#ifndef __OS2DEFS__H__
#define __OS2DEFS__H__
#include "my_global.h"

#if 0
#ifndef FALSE
#define FALSE         0
#define TRUE          1
#endif
#define CHAR    char            /* ch  */
#define SHORT   short           /* s   */
#if defined(BIT64)
typedef int     LONG;           /* i   */
#else    // BIT32
typedef long    LONG;           /* l   */
#endif   // BIT32
#define INT     int             /* i   */
#define VOID    void
#undef  HANDLE
typedef void*   LPVOID;
typedef unsigned short *LPWSTR;
typedef const unsigned short *LPCWSTR;

#if defined(UNIX) || defined(UNIV_LINUX) || defined(WIN32)
typedef int            bool;    /* f   */
#elif !defined(WIN)
typedef unsigned short bool;    /* f   */
#else
typedef unsigned short WBOOL;   /* f   */
#define bool           WBOOL
#endif

typedef unsigned char  UCHAR;   /* uch */
typedef unsigned short USHORT;  /* us  */
#if defined(BIT64)
typedef unsigned int   ULONG;   /* ul  */
#else    // BIT32
typedef unsigned long  ULONG;   /* ul  */
#endif   // BIT32
typedef unsigned int   uint;    /* ui  */
typedef unsigned char  BYTE;    /* b   */
typedef          char *PSZ;
typedef          char *PCH;
typedef const    char *LPCSTR;
typedef const    char *LPCTSTR;
typedef          char *LPSTR;
typedef          char *LPTSTR;
typedef CHAR   *PCHAR;
typedef SHORT  *PSHORT;
typedef ULONG   DWORD;
typedef USHORT  WORD;
#if defined(WIN)
typedef int   *WPLONG;
typedef BYTE   *WPBYTE;
typedef INT    *WPINT;
#define PINT   WPLONG
#define PBYTE   WPBYTE
#define PINT    WPINT
#else
//typedef int   *PINT;
typedef BYTE   *PBYTE;
typedef INT    *PINT;
#endif
typedef UCHAR  *PUCHAR;
typedef USHORT *PUSHORT;
typedef ULONG  *PULONG;
typedef uint   *PUINT;
typedef VOID   *PVOID;
typedef PVOID  *PPVOID;
typedef bool   *PBOOL;

#if defined(UNIX) || defined(UNIV_LINUX)
//#if !defined(MYSQL_DYNAMIC_PLUGIN)
#ifndef min
#define max(x,y)   (((x)>(y))?(x):(y))
#define min(x,y)   (((x)<(y))?(x):(y))
#endif   // !MYSQL_DYNAMIC_PLUGIN 
#define FAR
#define cdecl
#define _MAX_PATH   260
#define _MAX_DRIVE    3
#define _MAX_DIR    256
#define _MAX_FNAME  256
#define _MAX_EXT    256
#define INVALID_HANDLE_VALUE  (-1)
//#define FILE    file
//#if defined(LINUX)
//typedef void *HANDLE;
typedef int HANDLE;
//#else
#if !defined(MYSQL_DYNAMIC_PLUGIN) 
//typedef int  HANDLE;
#endif
//#endif   // !LINUX
typedef long long __int64;
//int getcwd(char *, int);
#define stricmp strcasecmp
#define strnicmp strncasecmp
#define _stricmp stricmp
#define _strnicmp strnicmp
#define _open open
#define MAX_PATH  256

#ifdef LINUX
//#define _fileno(p) __sfileno(p)     to be clarified
#endif // LINUX

#define __stdcall
#else  // !UNIX
typedef LPVOID HANDLE;
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
#if defined(WIN)
#define FAR    _far
#define NEAR   _near
#define PASCAL _pascal
#elif defined(WIN32)
#define FAR
#define NEAR
#else
#define PASCAL  pascal
#define FAR     far
#define NEAR    near
#endif
#endif  // !UNIX

typedef unsigned short SEL;     /* sel */
//typedef HANDLE  HWND;
#define HINSTANCE  HANDLE;

#if defined(OS32)
  typedef SEL *PSEL;

  /*** Useful Helper Macros */

  /* Create untyped far pointer from selector and offset */
  #define MAKEP( sel,off ) (( void * )( void * _Seg16 )( (sel) << 16 | (off) ))
  #define MAKE16P( sel,off ) (( void * _Seg16 )( (sel) << 16 | (off) ))

  /* Extract selector or offset from far pointer */
  #define SELECTOROF(ptr)     ((((ULONG)(ptr))>>13)|7)
  #define OFFSETOF(p)         (((PUSHORT)&(p))[0])
#else
typedef SEL FAR *PSEL;

/*** Useful Helper Macros */

/* Create untyped far pointer from selector and offset */
#define MAKEP(sel, off)     ((PVOID)MAKEULONG(off, sel))

/* Extract selector or offset from far pointer */
#if !defined(ODBC)
#define SELECTOROF(p)       (((PUSHORT)&(p))[1])
#define OFFSETOF(p)         (((PUSHORT)&(p))[0])
#endif

/* Combine l & h to form a 32 bit quantity. */
#define MAKEULONG(l, h)  ((ULONG)(((USHORT)(l)) | ((ULONG)((USHORT)(h))) << 16))
#endif

#if defined(UNIX) || defined(UNIV_LINUX)
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 1
#define FORMAT_MESSAGE_FROM_SYSTEM     2
#define FORMAT_MESSAGE_IGNORE_INSERTS  4
typedef const LPVOID LPCVOID;
#ifdef __cplusplus
extern "C" {
#endif

DWORD FormatMessage(
  DWORD dwFlags,
  LPCVOID lpSource,
  DWORD dwMessageId,
  DWORD dwLanguageId,
  LPSTR lpBuffer,
  DWORD nSize, ...
  );
unsigned long   _filelength(int);
void Sleep(DWORD);
#ifdef __cplusplus
}
#endif
#endif // UNIX
#endif // 0
#endif // ! __OS2DEFS__H__
/**************************** end of file **********************************/
