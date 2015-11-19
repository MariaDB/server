/************** PlugUtil C Program Source Code File (.C) ***************/
/*                                                                     */
/* PROGRAM NAME: PLUGUTIL                                              */
/* -------------                                                       */
/*  Version 2.9                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          1993-2015    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are initialization and utility Plug routines.         */
/*                                                                     */
/* WHAT YOU NEED TO COMPILE THIS PROGRAM:                              */
/* --------------------------------------                              */
/*                                                                     */
/*  REQUIRED FILES:                                                    */
/*  ---------------                                                    */
/*  See Readme.C for a list and description of required SYSTEM files.  */
/*                                                                     */
/*    PLUG.C          - Source code                                    */
/*    GLOBAL.H        - Global declaration file                        */
/*    OPTION.H        - Option declaration file                        */
/*                                                                     */
/*  REQUIRED LIBRARIES:                                                */
/*  -------------------                                                */
/*                                                                     */
/*    OS2.LIB        - OS2 libray                                      */
/*    LLIBCE.LIB     - Protect mode/standard combined large model C    */
/*                     library                                         */
/*                                                                     */
/*  REQUIRED PROGRAMS:                                                 */
/*  ------------------                                                 */
/*                                                                     */
/*    IBM C Compiler                                                   */
/*    IBM Linker                                                       */
/*                                                                     */
/***********************************************************************/

/***********************************************************************/
/*                                                                     */
/*  Include relevant MariaDB header file.                              */
/*                                                                     */
/***********************************************************************/
#include "my_global.h"
#if defined(__WIN__)
//#include <windows.h>
#else
#if defined(UNIX) || defined(UNIV_LINUX)
#include <errno.h>
#include <unistd.h>
//#define __stdcall
#else
#include <dir.h>
#endif
#include <stdarg.h>
#endif

#if defined(WIN)
#include <alloc.h>
#endif
#include <errno.h>                  /* definitions of ERANGE ENOMEM    */
#if !defined(UNIX) && !defined(UNIV_LINUX)
#include <direct.h>                 /* Directory management library    */
#endif

/***********************************************************************/
/*                                                                     */
/*  Include application header files                                   */
/*                                                                     */
/*  global.h     is header containing all global declarations.         */
/*                                                                     */
/***********************************************************************/
#define STORAGE                     /* Initialize global variables     */

#include "osutil.h"
#include "global.h"
#if defined(NEWMSG)
#include "rcmsg.h"
#endif   // NEWMSG

#if defined(__WIN__)
extern HINSTANCE s_hModule;                   /* Saved module handle    */
#endif   // __WIN__

#if defined(XMSG)
extern char *msg_path;
char *msglang(void);
#endif   // XMSG

/***********************************************************************/
/*                Local Definitions and static variables               */
/***********************************************************************/
typedef struct {
  ushort Segsize;
  ushort Size;
  } AREASIZE;

ACTIVITY defActivity = {            /* Describes activity and language */
  NULL,                             /* Points to user work area(s)     */
 "Unknown"};                        /* Application name                */

#if defined(XMSG) || defined(NEWMSG)
  static char stmsg[200];
#endif   // XMSG  ||         NEWMSG

#if defined(UNIX) || defined(UNIV_LINUX)
#include "rcmsg.h"
#endif   // UNIX

/**************************************************************************/
/*  Tracing output function.                                              */
/**************************************************************************/
void htrc(char const *fmt, ...)
  {
  va_list ap;
  va_start (ap, fmt);


//if (trace == 1)
//  vfprintf(debug, fmt, ap);
//else
    vfprintf(stderr, fmt, ap);

  va_end (ap);
  } // end of htrc

/***********************************************************************/
/*  Plug initialization routine.                                       */
/*  Language points on initial language name and eventual path.        */
/*  Return value is the pointer to the Global structure.               */
/***********************************************************************/
PGLOBAL PlugInit(LPCSTR Language, uint worksize)
  {
  PGLOBAL g;

  if (trace > 1)
    htrc("PlugInit: Language='%s'\n",
          ((!Language) ? "Null" : (char*)Language));

  if (!(g = malloc(sizeof(GLOBAL)))) {
    fprintf(stderr, MSG(GLOBAL_ERROR), (int)sizeof(GLOBAL));
    return NULL;
  } else {
    g->Sarea = NULL;
    g->Createas = 0;
    g->Alchecked = 0;
    g->Mrr = 0;
    g->Activityp = g->ActivityStart = NULL;
    g->Xchk = NULL;
    g->N = 0;
    strcpy(g->Message, "");

    /*******************************************************************/
    /*  Allocate the main work segment.                                */
    /*******************************************************************/
    if (worksize && !(g->Sarea = PlugAllocMem(g, worksize))) {
      char errmsg[256];
      sprintf(errmsg, MSG(WORK_AREA), g->Message);
      strcpy(g->Message, errmsg);
      g->Sarea_Size = 0;
    } else
      g->Sarea_Size = worksize;

  } /* endif g */

  g->jump_level = -1;   /* New setting to allow recursive call of Plug */
  return(g);
  } /* end of PlugInit */

/***********************************************************************/
/*  PlugExit: Terminate Plug operations.                               */
/***********************************************************************/
int PlugExit(PGLOBAL g)
  {
  int rc = 0;

  if (!g)
    return rc;

  if (g->Sarea)
    free(g->Sarea);

  free(g);
  return rc;
  } /* end of PlugExit */

/***********************************************************************/
/*  Remove the file type from a file name.                             */
/*  Note: this routine is not really implemented for Unix.             */
/***********************************************************************/
LPSTR PlugRemoveType(LPSTR pBuff, LPCSTR FileName)
  {
#if defined(__WIN__)
  char drive[_MAX_DRIVE];
#else
  char *drive = NULL;
#endif
  char direc[_MAX_DIR];
  char fname[_MAX_FNAME];
  char ftype[_MAX_EXT];

  _splitpath(FileName, drive, direc, fname, ftype);

  if (trace > 1) {
    htrc("after _splitpath: FileName=%s\n", FileName);
    htrc("drive=%s dir=%s fname=%s ext=%s\n",
          SVP(drive), direc, fname, ftype);
    } // endif trace

  _makepath(pBuff, drive, direc, fname, "");

  if (trace > 1)
    htrc("buff='%s'\n", pBuff);

  return pBuff;
  } // end of PlugRemoveType


BOOL PlugIsAbsolutePath(LPCSTR path)
{
#if defined(__WIN__)
  return ((path[0] >= 'a' && path[0] <= 'z') ||
          (path[0] >= 'A' && path[0] <= 'Z')) && path[1] == ':';
#else
  return path[0] == '/';
#endif
}

/***********************************************************************/
/*  Set the full path of a file relatively to a given path.            */
/*  Note: this routine is not really implemented for Unix.             */
/***********************************************************************/
LPCSTR PlugSetPath(LPSTR pBuff, LPCSTR prefix, LPCSTR FileName, LPCSTR defpath)
  {
  char newname[_MAX_PATH];
  char direc[_MAX_DIR], defdir[_MAX_DIR], tmpdir[_MAX_DIR];
  char fname[_MAX_FNAME];
  char ftype[_MAX_EXT];
#if defined(__WIN__)
  char drive[_MAX_DRIVE], defdrv[_MAX_DRIVE];
#else
  char *drive = NULL, *defdrv = NULL;
#endif

  if (!strncmp(FileName, "//", 2) || !strncmp(FileName, "\\\\", 2)) {
    strcpy(pBuff, FileName);       // Remote file
    return pBuff;
    } // endif

  if (PlugIsAbsolutePath(FileName))
  {
    strcpy(pBuff, FileName); // FileName includes absolute path
    return pBuff;
  } // endif
  
#if !defined(__WIN__)
  if (*FileName == '~') {
    if (_fullpath(pBuff, FileName, _MAX_PATH)) {
      if (trace > 1)
        htrc("pbuff='%s'\n", pBuff);

     return pBuff;
    } else
      return FileName;     // Error, return unchanged name
      
    } // endif FileName  
#endif   // !__WIN__
  
  if (prefix && strcmp(prefix, ".") && !PlugIsAbsolutePath(defpath))
  {
    char tmp[_MAX_PATH];
    int len= snprintf(tmp, sizeof(tmp) - 1, "%s%s%s",
                      prefix, defpath, FileName);
    memcpy(pBuff, tmp, (size_t) len);
    pBuff[len]= '\0';
    return pBuff;
  }

  _splitpath(FileName, drive, direc, fname, ftype);

  if (defpath) {
    char c = defpath[strlen(defpath) - 1];

    strcpy(tmpdir, defpath);

    if (c != '/' && c != '\\')
      strcat(tmpdir, "/");

  } else
    strcpy(tmpdir, "./");

  _splitpath(tmpdir, defdrv, defdir, NULL, NULL);

  if (trace > 1) {
    htrc("after _splitpath: FileName=%s\n", FileName);
#if defined(__WIN__)
    htrc("drive=%s dir=%s fname=%s ext=%s\n", drive, direc, fname, ftype);
    htrc("defdrv=%s defdir=%s\n", defdrv, defdir);
#else
    htrc("dir=%s fname=%s ext=%s\n", direc, fname, ftype);
#endif
    } // endif trace

  if (drive && !*drive)
    strcpy(drive, defdrv);

  switch (*direc) {
    case '\0':
      strcpy(direc, defdir);
      break;
    case '\\':
    case '/':
      break;
    default:
      // This supposes that defdir ends with a SLASH
      strcpy(direc, strcat(defdir, direc));
    } // endswitch

  _makepath(newname, drive, direc, fname, ftype);

  if (trace > 1)
    htrc("newname='%s'\n", newname);

  if (_fullpath(pBuff, newname, _MAX_PATH)) {
    if (trace > 1)
      htrc("pbuff='%s'\n", pBuff);

    return pBuff;
  } else
    return FileName;     // Error, return unchanged name

  } // end of PlugSetPath

#if defined(XMSG)
/***********************************************************************/
/*  PlugGetMessage: get a message from the message file.               */
/***********************************************************************/
char *PlugReadMessage(PGLOBAL g, int mid, char *m)
  {
  char  msgfile[_MAX_PATH], msgid[32], buff[256];
  char *msg;
  FILE *mfile = NULL;

//GetPrivateProfileString("Message", msglang, "Message\\english.msg",
//                                   msgfile, _MAX_PATH, plgini);
//strcat(strcat(strcpy(msgfile, msg_path), msglang()), ".msg");
  strcat(strcpy(buff, msglang()), ".msg");
  PlugSetPath(msgfile, NULL, buff, msg_path);

  if (!(mfile = fopen(msgfile, "rt"))) {
    sprintf(stmsg, "Fail to open message file %s", msgfile);
    goto err;
    } // endif mfile

  for (;;)
    if (!fgets(buff, 256, mfile)) {
      sprintf(stmsg, "Cannot get message %d %s", mid, SVP(m));
      goto fin;
    } else
      if (atoi(buff) == mid)
        break;

  if (sscanf(buff, " %*d %s \"%[^\"]", msgid, stmsg) < 2) {
    // Old message file
    if (!sscanf(buff, " %*d \"%[^\"]", stmsg)) {
      sprintf(stmsg, "Bad message file for %d %s", mid, SVP(m));
      goto fin;
    } else
      m = NULL;

    } // endif sscanf

  if (m && strcmp(m, msgid)) {
    // Message file is out of date
    strcpy(stmsg, m);
    goto fin;
    } // endif m

 fin:
  fclose(mfile);

 err:
  if (g) {
    // Called by STEP
    msg = PlugDup(g, stmsg);
  } else // Called by MSG or PlgGetErrorMsg
    msg =  stmsg;

  return msg;
  } // end of PlugReadMessage

#elif defined(NEWMSG)
/***********************************************************************/
/*  PlugGetMessage: get a message from the resource string table.      */
/***********************************************************************/
char *PlugGetMessage(PGLOBAL g, int mid)
  {
  char *msg;

#if 0 // was !defined(UNIX) && !defined(UNIV_LINUX)
  int   n = LoadString(s_hModule, (uint)mid, (LPTSTR)stmsg, 200);

  if (n == 0) {
    DWORD rc = GetLastError();
    msg = (char*)PlugSubAlloc(g, NULL, 512);   // Extend buf allocation
    n = sprintf(msg, "Message %d, rc=%d: ", mid, rc);
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS, NULL, rc, 0,
                  (LPTSTR)(msg + n), 512 - n, NULL);
    return msg;
    } // endif n

#else  // ALL
  if (!GetRcString(mid, stmsg, 200))
    sprintf(stmsg, "Message %d not found", mid);
#endif // ALL

  if (g) {
    // Called by STEP
    msg = PlugDup(g, stmsg);
  } else // Called by MSG or PlgGetErrorMsg
    msg =  stmsg;

  return msg;
  } // end of PlugGetMessage
#endif     // NEWMSG

#if defined(__WIN__)
/***********************************************************************/
/*  Return the line length of the console screen buffer.               */
/***********************************************************************/
short GetLineLength(PGLOBAL g)
  {
  CONSOLE_SCREEN_BUFFER_INFO coninfo;
  HANDLE  hcons = GetStdHandle(STD_OUTPUT_HANDLE);
  BOOL    b = GetConsoleScreenBufferInfo(hcons, &coninfo);

  return (b) ? coninfo.dwSize.X : 0;
  } // end of GetLineLength
#endif   // __WIN__

/***********************************************************************/
/*  Program for memory allocation of work and language areas.          */
/***********************************************************************/
void *PlugAllocMem(PGLOBAL g, uint size)
  {
  void *areap;                     /* Pointer to allocated area        */

  /*********************************************************************/
  /*  This is the allocation routine for the WIN32/UNIX/AIX version.   */
  /*********************************************************************/
  if (!(areap = malloc(size)))
    sprintf(g->Message, MSG(MALLOC_ERROR), "malloc");

  if (trace > 1) {
    if (areap)
      htrc("Memory of %u allocated at %p\n", size, areap);
    else
      htrc("PlugAllocMem: %s\n", g->Message);

    } // endif trace

  return (areap);
  } /* end of PlugAllocMem */

/***********************************************************************/
/*  Program for SubSet initialization of memory pools.                 */
/*  Here there should be some verification done such as validity of    */
/*  the address and size not larger than memory size.                  */
/***********************************************************************/
BOOL PlugSubSet(PGLOBAL g __attribute__((unused)), void *memp, uint size)
  {
  PPOOLHEADER pph = memp;

  pph->To_Free = (OFFSET)sizeof(POOLHEADER);
  pph->FreeBlk = size - pph->To_Free;

  return FALSE;
  } /* end of PlugSubSet */

/***********************************************************************/
/*  Program for sub-allocating one item in a storage area.             */
/*  Note: SubAlloc routines of OS/2 are no more used to increase the   */
/*  code portability and avoid problems when a grammar compiled under  */
/*  one version of OS/2 is used under another version.                 */
/*  The simple way things are done here is also based on the fact      */
/*  that no freeing of suballocated blocks is permitted in Plug.       */
/***********************************************************************/
void *PlugSubAlloc(PGLOBAL g, void *memp, size_t size)
  {
  PPOOLHEADER pph;                           /* Points on area header. */

  if (!memp)
    /*******************************************************************/
    /*  Allocation is to be done in the Sarea.                         */
    /*******************************************************************/
    memp = g->Sarea;

//size = ((size + 3) / 4) * 4;       /* Round up size to multiple of 4 */
  size = ((size + 7) / 8) * 8;       /* Round up size to multiple of 8 */
  pph = (PPOOLHEADER)memp;

  if (trace > 3)
    htrc("SubAlloc in %p size=%d used=%d free=%d\n",
          memp, size, pph->To_Free, pph->FreeBlk);

  if ((uint)size > pph->FreeBlk) {   /* Not enough memory left in pool */
    char     *pname = "Work";

    sprintf(g->Message,
      "Not enough memory in %s area for request of %u (used=%d free=%d)",
                          pname, (uint) size, pph->To_Free, pph->FreeBlk);

    if (trace)
      htrc("PlugSubAlloc: %s\n", g->Message);

    longjmp(g->jumper[g->jump_level], 1);
    } /* endif size OS32 code */

  /*********************************************************************/
  /*  Do the suballocation the simplest way.                           */
  /*********************************************************************/
  memp = MakePtr(memp, pph->To_Free); /* Points to suballocated block  */
  pph->To_Free += size;               /* New offset of pool free block */
  pph->FreeBlk -= size;               /* New size   of pool free block */

  if (trace > 3)
    htrc("Done memp=%p used=%d free=%d\n",
          memp, pph->To_Free, pph->FreeBlk);

  return (memp);
  } /* end of PlugSubAlloc */

/***********************************************************************/
/*  Program for sub-allocating and copying a string in a storage area. */
/***********************************************************************/
char *PlugDup(PGLOBAL g, const char *str)
  {
  if (str) {
    char *sm = (char*)PlugSubAlloc(g, NULL, strlen(str) + 1);

    strcpy(sm, str);
    return sm;
  } else
    return NULL;

  } // end of PlugDup 

#if 0
/***********************************************************************/
/* This routine suballocate a copy of the passed string.               */
/***********************************************************************/
char *PlugDup(PGLOBAL g, const char *str)
  {
  char  *buf;
  size_t len;

  if (str && (len = strlen(str))) {
    buf = (char*)PlugSubAlloc(g, NULL, len + 1);
    strcpy(buf, str);
  } else
    buf = NULL;

  return(buf);
  } /* end of PlugDup */
#endif // 0

/***********************************************************************/
/* This routine makes a pointer from an offset to a memory pointer.    */
/***********************************************************************/
void *MakePtr(void *memp, OFFSET offset)
  {
  return ((offset == 0) ? NULL : &((char *)memp)[offset]);
  } /* end of MakePtr */

/***********************************************************************/
/* This routine makes an offset from a pointer new format.             */
/***********************************************************************/
#if 0
OFFSET MakeOff(void *memp, void *ptr)
  {
  return ((!ptr) ? 0 : (OFFSET)((char *)ptr - (char *)memp));
  } /* end of MakeOff */
#endif
/*--------------------- End of PLUGUTIL program -----------------------*/
