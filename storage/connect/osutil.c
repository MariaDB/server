/* Copyright (C) MariaDB Corporation Ab */
#include "my_global.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "osutil.h"

#ifdef _WIN32
my_bool CloseFileHandle(HANDLE h) 
  {
  return !CloseHandle(h);
  } /* end of CloseFileHandle */

#else  /* UNIX */
/* code to handle Linux and Solaris */
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>
#include <fcntl.h>
#include <pwd.h>

extern FILE *debug;

/***********************************************************************/
/*  Define some functions not existing in the UNIX library.            */
/***********************************************************************/
PSZ strupr(PSZ p)
  {
  register int i;

  for (i = 0; p[i]; i++)
    p[i] = toupper(p[i]);

  return (p);
  } /* end of strupr */

PSZ strlwr(PSZ p)
  {
  register int i;

  for (i = 0; p[i]; i++)
    p[i] = tolower(p[i]);

  return (p);
  } /* end of strlwr */

/***********************************************************************/
/*  Define the splitpath function not existing in the UNIX library.    */
/***********************************************************************/
void _splitpath(LPCSTR name, LPSTR drive, LPSTR dir, LPSTR fn, LPSTR ft)
  {
  LPCSTR p2, p = name;

#ifdef DEBTRACE
 htrc("SplitPath: name=%s [%s (%d)]\n", 
   XSTR(name), XSTR(__FILE__), __LINE__);
#endif

  if (drive) *drive = '\0';
  if (dir) *dir = '\0';
  if (fn) *fn = '\0';
  if (ft) *ft = '\0';

  if ((p2 = strrchr(p, '/'))) {
    p2++;
    if (dir) strncat(dir, p, p2 - p);
    p = p2;
    } /* endif p2 */

  if ((p2 = strrchr(p, '.'))) {
    if (fn) strncat(fn, p, p2 - p);
    if (ft) strcpy(ft, p2);
  } else
    if (fn) strcpy(fn, p);

#ifdef DEBTRACE
 htrc("SplitPath: name=%s drive=%s dir=%s filename=%s type=%s [%s(%d)]\n",
  XSTR(name), XSTR(drive), XSTR(dir), XSTR(fn), XSTR(ft), __FILE__, __LINE__);
#endif
  } /* end of _splitpath */

/***********************************************************************/
/*  Define the makepath function not existing in the UNIX library.     */
/***********************************************************************/
void _makepath(LPSTR name, LPCSTR drive __attribute__((unused)), LPCSTR dir, LPCSTR fn, LPCSTR ft)
  {
  int n;

  if (!name)
    return;
  else
    *name = '\0';

  if (dir && (n = strlen(dir)) > 0) {
    strcpy(name, dir);

    if (name[n-1] != '/')
      strcat(name, "/");

    }  /* endif dir */

  if (fn)
    strcat(name, fn);

  if (ft && strlen(ft)) {
    if (*ft != '.')
      strcat(name, ".");

    strcat(name, ft);
    } /* endif ft */

  } /* end of _makepath */

my_bool CloseFileHandle(HANDLE h) 
  {
  return (close(h)) ? TRUE : FALSE;
  }  /* end of CloseFileHandle */

int GetLastError() 
  {
  return errno;
  }  /* end of GetLastError */

unsigned long _filelength(int fd) 
  {
  struct stat st;

  if (fd == -1)
    return 0;

  if (fstat(fd, &st) != 0)
    return 0;

  return st.st_size;
  }  /* end of _filelength */

char *_fullpath(char *absPath, const char *relPath, size_t maxLength)
  {
  // Fixme
  char *p;

  if ( *relPath == '\\' || *relPath == '/' ) {
    strncpy(absPath, relPath, maxLength);
  } else if (*relPath == '~') {
    // get the path to the home directory
    struct passwd *pw = getpwuid(getuid());
    const char    *homedir = pw->pw_dir;

    if (homedir)
      strcat(strncpy(absPath, homedir, maxLength), relPath + 1);
    else
      strncpy(absPath, relPath, maxLength);
        
  } else {
    char buff[2*_MAX_PATH];

    p= getcwd(buff, _MAX_PATH);
    assert(p);
    strcat(buff,"/");
    strcat(buff, relPath);
    strncpy(absPath, buff, maxLength);
  }  /* endif's relPath */

  p = absPath;

  for(; *p; p++)
    if (*p == '\\')
      *p = '/';

  return absPath;
  }  /* end of _fullpath */

BOOL MessageBeep(uint i __attribute__((unused)))
  {
  // Fixme
  return TRUE;
  } /* end of MessageBeep */

#endif  // UNIX
