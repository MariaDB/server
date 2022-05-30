/************** PlugUtil C Program Source Code File (.C) ***************/
/*                                                                     */
/* PROGRAM NAME: PLUGUTIL                                              */
/* -------------                                                       */
/*  Version 3.1                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          1993-2020    */
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
#if defined(_WIN32)
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
#include "plgdbsem.h"
#if defined(NEWMSG)
#include "rcmsg.h"
#endif   // NEWMSG

#if defined(_WIN32)
extern HINSTANCE s_hModule;                   /* Saved module handle    */
#endif   // _WIN32

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
/*  Conditional tracing output function.                                  */
/**************************************************************************/
void xtrc(uint x, char const *fmt, ...)
{
	if (GetTraceValue() & x) {
		va_list ap;
		va_start(ap, fmt);

		vfprintf(stderr, fmt, ap);
		va_end(ap);
	} // endif x

} // end of xtrc

/**************************************************************************/
/*  Tracing output function.                                              */
/**************************************************************************/
void htrc(char const* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);

	vfprintf(stderr, fmt, ap);
	va_end(ap);
} // end of htrc

/***********************************************************************/
/*  Plug initialization routine.                                       */
/*  Language points on initial language name and eventual path.        */
/*  Return value is the pointer to the Global structure.               */
/***********************************************************************/
PGLOBAL PlugInit(LPCSTR Language, size_t worksize)
{
	PGLOBAL g;

	if (trace(2))
		htrc("PlugInit: Language='%-.256s'\n",
			((!Language) ? "Null" : (char*)Language));

	try {
		g = new GLOBAL;
	} catch (...) {
		fprintf(stderr, MSG(GLOBAL_ERROR), (int)sizeof(GLOBAL));
		return NULL;
	} // end try/catch

	g->Sarea = NULL;
	g->Createas = false;
	g->Alchecked = 0;
	g->Mrr = 0;
	g->Activityp = NULL;
	g->Xchk = NULL;
	g->N = 0;
	g->More = 0;
	g->Saved_Size = 0;
	strcpy(g->Message, "");

	/*******************************************************************/
	/*  Allocate the main work segment.                                */
	/*******************************************************************/
	if (worksize && AllocSarea(g, worksize)) {
		char errmsg[MAX_STR];
		snprintf(errmsg, sizeof(errmsg) - 1, MSG(WORK_AREA), g->Message);
		strcpy(g->Message, errmsg);
	} // endif Sarea

	g->jump_level = -1;   /* New setting to allow recursive call of Plug */
	return(g);
} /* end of PlugInit */

/***********************************************************************/
/*  PlugExit: Terminate Plug operations.                               */
/***********************************************************************/
PGLOBAL PlugExit(PGLOBAL g)
{
	if (g) {
		PDBUSER dup = PlgGetUser(g);

		if (dup)
			free(dup);

		FreeSarea(g);
		delete g;
	}	// endif g

  return NULL;
} // end of PlugExit

/***********************************************************************/
/*  Remove the file type from a file name.                             */
/*  Note: this routine is not really implemented for Unix.             */
/***********************************************************************/
LPSTR PlugRemoveType(LPSTR pBuff, LPCSTR FileName)
{
#if defined(_WIN32)
  char drive[_MAX_DRIVE];
#else
  char *drive = NULL;
#endif
  char direc[_MAX_DIR];
  char fname[_MAX_FNAME];
  char ftype[_MAX_EXT];

  _splitpath(FileName, drive, direc, fname, ftype);

  if (trace(2)) {
    htrc("after _splitpath: FileName=%-.256s\n", FileName);
    htrc("drive=%-.256s dir=%-.256s fname=%-.256s ext=%-.256s\n",
          SVP(drive), direc, fname, ftype);
    } // endif trace

  _makepath(pBuff, drive, direc, fname, "");

  if (trace(2))
    htrc("buff='%-.256s'\n", pBuff);

  return pBuff;
} // end of PlugRemoveType

BOOL PlugIsAbsolutePath(LPCSTR path)
{
#if defined(_WIN32)
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
#if defined(_WIN32)
  char drive[_MAX_DRIVE], defdrv[_MAX_DRIVE];
#else
  char *drive = NULL, *defdrv = NULL;
#endif

	if (trace(2))
		htrc("prefix=%-.256s fn=%-.256s path=%-.256s\n", prefix, FileName, defpath);

	if (strlen(FileName) >= _MAX_PATH)
	{
		*pBuff= 0; /* Hope this is treated as error of some kind*/
		return FileName;
	}

  if (!strncmp(FileName, "//", 2) || !strncmp(FileName, "\\\\", 2)) {
    strcpy(pBuff, FileName);       // Remote file
    return pBuff;
    } // endif

  if (PlugIsAbsolutePath(FileName))
  {
    strcpy(pBuff, FileName); // FileName includes absolute path
    return pBuff;
  } // endif
  
#if !defined(_WIN32)
  if (*FileName == '~') {
    if (_fullpath(pBuff, FileName, _MAX_PATH)) {
      if (trace(2))
        htrc("pbuff='%-.256s'\n", pBuff);

     return pBuff;
    } else
      return FileName;     // Error, return unchanged name
      
    } // endif FileName  
#endif   // !_WIN32
  
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

  if (trace(2)) {
    htrc("after _splitpath: FileName=%-.256s\n", FileName);
#if defined(_WIN32)
    htrc("drive=%-.256s dir=%-.256s fname=%-.256s ext=%-.256s\n", drive, direc, fname, ftype);
    htrc("defdrv=%-.256s defdir=%-.256s\n", defdrv, defdir);
#else
    htrc("dir=%-.256s fname=%-.256s ext=%-.256s\n", direc, fname, ftype);
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

  if (trace(2))
    htrc("newname='%-.256s'\n", newname);

  if (_fullpath(pBuff, newname, _MAX_PATH)) {
    if (trace(2))
      htrc("pbuff='%-.256s'\n", pBuff);

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
    sprintf(stmsg, "Fail to open message file %-.256s", msgfile);
    goto err;
    } // endif mfile

  for (;;)
    if (!fgets(buff, 256, mfile)) {
      sprintf(stmsg, "Cannot get message %d %-.256s", mid, SVP(m));
      goto fin;
    } else
      if (atoi(buff) == mid)
        break;

  if (sscanf(buff, " %*d %.31s \"%.255[^\"]", msgid, stmsg) < 2) {
    // Old message file
    if (!sscanf(buff, " %*d \"%.255[^\"]", stmsg)) {
      sprintf(stmsg, "Bad message file for %d %-.256s", mid, SVP(m));
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

#if defined(_WIN32)
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
#endif   // _WIN32

/***********************************************************************/
/*  Program for memory allocation of work and language areas.          */
/***********************************************************************/
bool AllocSarea(PGLOBAL g, size_t size)
{
  /*********************************************************************/
  /*  This is the allocation routine for the WIN32/UNIX/AIX version.   */
  /*********************************************************************/
#if defined(_WIN32)
	if (size >= 1048576)			 // 1M
		g->Sarea = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	else
#endif
		g->Sarea = malloc(size);

	if (!g->Sarea) {
		sprintf(g->Message, MSG(MALLOC_ERROR), "malloc");
		g->Sarea_Size = 0;
	}	else {
    g->Sarea_Size = size;
    PlugSubSet(g->Sarea, size);
  } // endif Sarea

#if defined(DEVELOPMENT)
	if (true) {
#else
	if (trace(8)) {
#endif
    if (g->Sarea) {
      htrc("Work area of %zd allocated at %p\n", size, g->Sarea);
    } else
      htrc("SareaAlloc: %-.256s\n", g->Message);

  } // endif trace

  return (!g->Sarea);
} // end of AllocSarea

/***********************************************************************/
/*  Program for memory freeing the work area.                          */
/***********************************************************************/
void FreeSarea(PGLOBAL g)
{
	if (g->Sarea) {
#if defined(_WIN32)
		if (g->Sarea_Size >= 1048576)			 // 1M
			VirtualFree(g->Sarea, 0, MEM_RELEASE);
		else
#endif
			free(g->Sarea);

#if defined(DEVELOPMENT)
		if (true)
#else
		if (trace(8))
#endif
			htrc("Freeing Sarea at %p size = %zd\n", g->Sarea, g->Sarea_Size);

		g->Sarea = NULL;
		g->Sarea_Size = 0;
	} // endif Sarea

	return;
} // end of FreeSarea

/***********************************************************************/
/*  Program for SubSet initialization of memory pools.                 */
/*  Here there should be some verification done such as validity of    */
/*  the address and size not larger than memory size.                  */
/***********************************************************************/
BOOL PlugSubSet(void *memp, size_t size)
{
  PPOOLHEADER pph = (PPOOLHEADER)memp;

  pph->To_Free = (size_t)sizeof(POOLHEADER);
  pph->FreeBlk = size - pph->To_Free;
  return FALSE;
} /* end of PlugSubSet */

/***********************************************************************/
/*  Use it to export a function that do throwing.                      */
/***********************************************************************/
static void *DoThrow(int n)
{
	throw n;
} /* end of DoThrow */

/***********************************************************************/
/*  Program for sub-allocating one item in a storage area.             */
/*  The simple way things are done here is based on the fact           */
/*  that no freeing of suballocated blocks is permitted in CONNECT.    */
/***********************************************************************/
void *PlugSubAlloc(PGLOBAL g, void *memp, size_t size)
{
	PPOOLHEADER pph;                           /* Points on area header. */

  if (!memp)
    /*******************************************************************/
    /*  Allocation is to be done in the Sarea.                         */
    /*******************************************************************/
    memp = g->Sarea;

  size = ((size + 7) / 8) * 8;       /* Round up size to multiple of 8 */
  pph = (PPOOLHEADER)memp;

  if (trace(16))
    htrc("SubAlloc in %p size=%zd used=%zd free=%zd\n",
          memp, size, pph->To_Free, pph->FreeBlk);

  if (size > pph->FreeBlk) {   /* Not enough memory left in pool */
    PCSZ pname = "Work";

    sprintf(g->Message,
      "Not enough memory in %-.256s area for request of %zu (used=%zu free=%zu)",
                          pname, size, pph->To_Free, pph->FreeBlk);

    if (trace(1))
      htrc("PlugSubAlloc: %-.256s\n", g->Message);

    DoThrow(1234);
  } /* endif size OS32 code */

  /*********************************************************************/
  /*  Do the suballocation the simplest way.                           */
  /*********************************************************************/
  memp = MakePtr(memp, pph->To_Free); /* Points to suballocated block  */
  pph->To_Free += size;               /* New offset of pool free block */
  pph->FreeBlk -= size;               /* New size   of pool free block */

  if (trace(16))
    htrc("Done memp=%p used=%zd free=%zd\n",
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

/*************************************************************************/
/* This routine makes a pointer from an offset to a memory pointer.      */
/*************************************************************************/
void* MakePtr(void* memp, size_t offset)
{
  // return ((offset == 0) ? NULL : &((char*)memp)[offset]);
  return (!offset) ? NULL : (char *)memp + offset;
} /* end of MakePtr */

/*************************************************************************/
/* This routine makes an offset from a pointer new format.               */
/*************************************************************************/
size_t MakeOff(void* memp, void* ptr)
{
  if (ptr) {
#if defined(_DEBUG) || defined(DEVELOPMENT)
    if (ptr <= memp) {
      fprintf(stderr, "ptr %p <= memp %p", ptr, memp);
      DoThrow(999);
    } // endif ptr
#endif   // _DEBUG  ||         DEVELOPMENT
    return (size_t)(((char*)ptr) - ((char*)memp));
  } else
    return 0;

} /* end of MakeOff */

/*---------------------- End of PLUGUTIL program ------------------------*/
