/********** PlgDBUtl Fpe C++ Program Source Code File (.CPP) ***********/
/* PROGRAM NAME: PLGDBUTL                                              */
/* -------------                                                       */
/*  Version 3.9                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          1998-2015    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  Utility functions used by DB semantic routines.                    */
/*                                                                     */
/* WHAT YOU NEED TO COMPILE THIS PROGRAM:                              */
/* --------------------------------------                              */
/*                                                                     */
/*  REQUIRED FILES:                                                    */
/*  ---------------                                                    */
/*  See Readme.C for a list and description of required SYSTEM files.  */
/*                                                                     */
/*    PLGDBUTL.C     - Source code                                     */
/*    GLOBAL.H       - Global declaration file                         */
/*    PLGDBSEM.H     - DB application declaration file                 */
/*                                                                     */
/*  REQUIRED LIBRARIES:                                                */
/*  -------------------                                                */
/*    OS2.LIB        - OS2 libray                                      */
/*    LLIBCE.LIB     - Protect mode/standard combined large model C    */
/*                     library                                         */
/*                                                                     */
/*  REQUIRED PROGRAMS:                                                 */
/*  ------------------                                                 */
/*    IBM, MS, Borland or GNU C++ Compiler                             */
/*    IBM, MS, Borland or GNU Linker                                   */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include "my_global.h"
#if defined(__WIN__)
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#define BIGMEM         1048576            // 1 Megabyte
#else     // !__WIN__
#include <unistd.h>
#include <fcntl.h>
#if defined(THREAD)
#include <pthread.h>
#endif   // THREAD
#include <stdarg.h>
#define BIGMEM      2147483647            // Max int value
#endif    // !__WIN__
#include <locale.h>

/***********************************************************************/
/*  Include application header files                                   */
/***********************************************************************/
#include "global.h"    // header containing all global declarations.
#include "plgdbsem.h"  // header containing the DB applic. declarations.
#include "preparse.h"  // For DATPAR
#include "osutil.h"
#include "maputil.h"
#include "catalog.h"
#include "colblk.h"
#include "xtable.h"    // header of TBX, TDB and TDBASE classes
#include "tabcol.h"    // header of XTAB and COLUMN classes
#include "valblk.h"
#include "rcmsg.h"

/***********************************************************************/
/*  Macro or external routine definition                               */
/***********************************************************************/
#if defined(THREAD)
#if defined(__WIN__)
extern CRITICAL_SECTION parsec;      // Used calling the Flex parser
#else   // !__WIN__
extern pthread_mutex_t parmut;
#endif  // !__WIN__
#endif  //  THREAD

/***********************************************************************/
/*  DB static variables.                                               */
/***********************************************************************/
bool  Initdone = false;
bool  plugin = false;  // True when called by the XDB plugin handler 

extern "C" {
extern char version[];
} // extern "C"

// The debug trace used by the main thread
       FILE *pfile = NULL;

MBLOCK Nmblk = {NULL, false, 0, false, NULL};   // Used to init MBLOCK's

/***********************************************************************/
/*  Routines called externally and internally by utility routines.     */
/***********************************************************************/
bool PlugEvalLike(PGLOBAL, LPCSTR, LPCSTR, bool);
bool EvalLikePattern(LPCSTR, LPCSTR);
void PlugConvertConstant(PGLOBAL, void* &, short&);

#ifdef DOMDOC_SUPPORT
void CloseXMLFile(PGLOBAL, PFBLOCK, bool);
#endif   // DOMDOC_SUPPORT

#ifdef LIBXML2_SUPPORT
#include "libdoc.h"
#endif   // LIBXML2_SUPPORT


/***********************************************************************/
/* Routines for file IO with error reporting to g->Message             */
/* Note: errno and strerror must be called before the message file     */
/* is read in the case of XMSG compile.                                */
/***********************************************************************/
static void global_open_error_msg(GLOBAL *g, int msgid, const char *path, 
                                                        const char *mode)
{
  int  len, rno= (int)errno;
  char errmsg[256]= "";

  strncat(errmsg, strerror(errno), 255);

  switch (msgid)
  {
    case MSGID_CANNOT_OPEN:
      len= snprintf(g->Message, sizeof(g->Message) - 1,
                    MSG(CANNOT_OPEN), // Cannot open %s
                    path);
      break;

    case MSGID_OPEN_MODE_ERROR:
      len= snprintf(g->Message, sizeof(g->Message) - 1,
                    MSG(OPEN_MODE_ERROR), // "Open(%s) error %d on %s"
                    mode, rno, path);
      break;

    case MSGID_OPEN_MODE_STRERROR:
      {char fmt[256];
      strcat(strcpy(fmt, MSG(OPEN_MODE_ERROR)), ": %s");
      len= snprintf(g->Message, sizeof(g->Message) - 1,
                    fmt, // Open(%s) error %d on %s: %s
                    mode, rno, path, errmsg);
      }break;

    case MSGID_OPEN_STRERROR:
      len= snprintf(g->Message, sizeof(g->Message) - 1,
                    MSG(OPEN_STRERROR), // "open error: %s"
                    errmsg);
      break;

    case MSGID_OPEN_ERROR_AND_STRERROR:
      len= snprintf(g->Message, sizeof(g->Message) - 1,
                    //OPEN_ERROR does not work, as it wants mode %d (not %s)
                    //MSG(OPEN_ERROR) "%s",// "Open error %d in mode %d on %s: %s"
                    "Open error %d in mode %s on %s: %s",
                    rno, mode, path, errmsg);
      break;

    case MSGID_OPEN_EMPTY_FILE:
      len= snprintf(g->Message, sizeof(g->Message) - 1,
                    MSG(OPEN_EMPTY_FILE), // "Opening empty file %s: %s"
                    path, errmsg);
      break;

    default:
      DBUG_ASSERT(0);
      /* Fall through*/
    case 0:
      len= 0;
  }
  g->Message[len]= '\0';
}


FILE *global_fopen(GLOBAL *g, int msgid, const char *path, const char *mode)
{
  FILE *f;
  if (!(f= fopen(path, mode)))
    global_open_error_msg(g, msgid, path, mode);
  return f;
}


int global_open(GLOBAL *g, int msgid, const char *path, int flags)
{
  int h;
  if ((h= open(path, flags)) <= 0)
    global_open_error_msg(g, msgid, path, "");
  return h;
}


int global_open(GLOBAL *g, int msgid, const char *path, int flags, int mode)
{
  int h;
  if ((h= open(path, flags, mode)) <= 0)
  {
    char modestr[64];
    snprintf(modestr, sizeof(modestr), "%d", mode);
    global_open_error_msg(g, msgid, path, modestr);
  }
  return h;
}

DllExport void SetTrc(void)
  {
  // If tracing is on, debug must be initialized.
  debug = pfile;
  } // end of SetTrc

#if 0
/**************************************************************************/
/*  Tracing output function.                                              */
/**************************************************************************/
void ptrc(char const *fmt, ...)
  {
  va_list ap;
  va_start (ap, fmt);

//  if (trace == 0 || (trace == 1 && !pfile) || !fmt)
//    printf("In %s wrong trace=%d pfile=%p fmt=%p\n", 
//      __FILE__, trace, pfile, fmt);

  if (trace == 1)
    vfprintf(pfile, fmt, ap);
  else
    vprintf(fmt, ap);

  va_end (ap);
  } // end of ptrc
#endif // 0

/**************************************************************************/
/*  Allocate the result structure that will contain result data.          */
/**************************************************************************/
PQRYRES PlgAllocResult(PGLOBAL g, int ncol, int maxres, int ids,
                       int *buftyp, XFLD *fldtyp, 
                       unsigned int *length, bool blank, bool nonull)
  {
  char     cname[NAM_LEN+1];
  int      i;
  PCOLRES *pcrp, crp;
  PQRYRES  qrp;

  // Save stack and allocation environment and prepare error return
  if (g->jump_level == MAX_JUMP) {
    strcpy(g->Message, MSG(TOO_MANY_JUMPS));
    return NULL;
    } // endif jump_level

  if (setjmp(g->jumper[++g->jump_level]) != 0) {
    printf("%s\n", g->Message);
    qrp = NULL;
    goto fin;
    } // endif rc

  /************************************************************************/
  /*  Allocate the structure used to contain the result set.              */
  /************************************************************************/
  qrp = (PQRYRES)PlugSubAlloc(g, NULL, sizeof(QRYRES));
  pcrp = &qrp->Colresp;
  qrp->Continued = false;
  qrp->Truncated = false;
  qrp->Info = false;
  qrp->Suball = true;
  qrp->Maxres = maxres;
  qrp->Maxsize = 0;
  qrp->Nblin = 0;
  qrp->Nbcol = 0;                                     // will be ncol
  qrp->Cursor = 0;
  qrp->BadLines = 0;

  for (i = 0; i < ncol; i++) {
    *pcrp = (PCOLRES)PlugSubAlloc(g, NULL, sizeof(COLRES));
    crp = *pcrp;
    pcrp = &crp->Next;
    memset(crp, 0, sizeof(COLRES));
    crp->Colp = NULL;
    crp->Ncol = ++qrp->Nbcol;
    crp->Type = buftyp[i];
    crp->Length = length[i];
    crp->Clen = GetTypeSize(crp->Type, length[i]);
    crp->Prec = 0;

    if (ids > 0) {
#if defined(XMSG)
      // Get header from message file
			strncpy(cname, PlugReadMessage(g, ids + crp->Ncol, NULL), NAM_LEN);
			cname[NAM_LEN] = 0;					// for truncated long names
#else   // !XMSG
      GetRcString(ids + crp->Ncol, cname, sizeof(cname));
#endif  // !XMSG
      crp->Name = (PSZ)PlugDup(g, cname);
    } else
      crp->Name = NULL;           // Will be set by caller

    if (fldtyp)
      crp->Fld = fldtyp[i];
    else
      crp->Fld = FLD_NO;

    // Allocate the Value Block that will contain data
    if (crp->Length || nonull)
      crp->Kdata = AllocValBlock(g, NULL, crp->Type, maxres,
                                    crp->Length, 0, true, blank, false);
    else
      crp->Kdata = NULL;

    if (trace)
      htrc("Column(%d) %s type=%d len=%d value=%p\n",
              crp->Ncol, crp->Name, crp->Type, crp->Length, crp->Kdata);

    } // endfor i

  *pcrp = NULL;

 fin:
  g->jump_level--;
  return qrp;
  } // end of PlgAllocResult

/***********************************************************************/
/*  Allocate and initialize the new DB User Block.                     */
/***********************************************************************/
PDBUSER PlgMakeUser(PGLOBAL g)
  {
  PDBUSER dbuserp;

  if (!(dbuserp = (PDBUSER)PlugAllocMem(g, (uint)sizeof(DBUSERBLK)))) {
    sprintf(g->Message, MSG(MALLOC_ERROR), "PlgMakeUser");
    return NULL;
    } // endif dbuserp

  memset(dbuserp, 0, sizeof(DBUSERBLK));
  dbuserp->Maxbmp = MAXBMP;
//dbuserp->UseTemp = TMP_AUTO;
  dbuserp->Check = CHK_ALL;
  strcpy(dbuserp->Server, "CONNECT");
  return dbuserp;
  } // end of PlgMakeUser

/***********************************************************************/
/*  PlgGetUser: returns DBUSER block pointer.                          */
/***********************************************************************/
PDBUSER PlgGetUser(PGLOBAL g)
  {
  PDBUSER dup = (PDBUSER)((g->Activityp) ? g->Activityp->Aptr : NULL);

  if (!dup)
    strcpy(g->Message, MSG(APPL_NOT_INIT));

  return dup;
  } // end of PlgGetUser

/***********************************************************************/
/*  PlgGetCatalog: returns CATALOG class pointer.                      */
/***********************************************************************/
PCATLG PlgGetCatalog(PGLOBAL g, bool jump)
  {
  PDBUSER dbuserp = PlgGetUser(g);
  PCATLG  cat = (dbuserp) ? dbuserp->Catalog : NULL;

  if (!cat && jump) {
    // Raise exception so caller doesn't have to check return value
    strcpy(g->Message, MSG(NO_ACTIVE_DB));
    longjmp(g->jumper[g->jump_level], 1);
    } // endif cat

  return cat;
  } // end of PlgGetCatalog

#if 0
/***********************************************************************/
/*  PlgGetDataPath: returns the default data path.                     */
/***********************************************************************/
char *PlgGetDataPath(PGLOBAL g)
  {
  PCATLG cat = PlgGetCatalog(g, false);

  return (cat) ? cat->GetDataPath() : NULL;
  } // end of PlgGetDataPath
#endif // 0

/***********************************************************************/
/*  This function returns a database path.                             */
/***********************************************************************/
char *SetPath(PGLOBAL g, const char *path)
{
  char *buf= NULL;

	if (path) {
		size_t len= strlen(path) + (*path != '.' ? 4 : 1);

		buf= (char*)PlugSubAlloc(g, NULL, len);
		
		if (PlugIsAbsolutePath(path)) {
		  strcpy(buf, path);
		  return buf;
		  } // endif path

		if (*path != '.') {
#if defined(__WIN__)
			char *s= "\\";
#else   // !__WIN__
			char *s= "/";
#endif  // !__WIN__
			strcat(strcat(strcat(strcpy(buf, "."), s), path), s);
		} else
			strcpy(buf, path);

		} // endif path

  return buf;
} // end of SetPath

/***********************************************************************/
/*  Extract from a path name the required component.                   */
/*  This function assumes there is enough space in the buffer.         */
/***********************************************************************/
char *ExtractFromPath(PGLOBAL g, char *pBuff, char *FileName, OPVAL op)
  {
  char *drive = NULL, *direc = NULL, *fname = NULL, *ftype = NULL;

  switch (op) {           // Determine which part to extract
#if defined(__WIN__)
    case OP_FDISK: drive = pBuff; break;
#endif   // !UNIX
    case OP_FPATH: direc = pBuff; break;
    case OP_FNAME: fname = pBuff; break;
    case OP_FTYPE: ftype = pBuff; break;
    default:
      sprintf(g->Message, MSG(INVALID_OPER), op, "ExtractFromPath");
      return NULL;
    } // endswitch op

  // Now do the extraction
  _splitpath(FileName, drive, direc, fname, ftype);
  return pBuff;
  } // end of PlgExtractFromPath

/***********************************************************************/
/*  Check the occurence and matching of a pattern against a string.    */
/*  Because this function is only used for catalog name checking,      */
/*  it must be case insensitive.                                       */
/***********************************************************************/
static bool PlugCheckPattern(PGLOBAL g, LPCSTR string, LPCSTR pat)
  {
  if (pat && strlen(pat)) {
    // This leaves 512 bytes (MAX_STR / 2) for each components
    LPSTR name = g->Message + MAX_STR / 2;

    strlwr(strcpy(name, string));
    strlwr(strcpy(g->Message, pat));         // Can be modified by Eval
    return EvalLikePattern(name, g->Message);
  } else
    return true;

  } // end of PlugCheckPattern

/***********************************************************************/
/*  PlugEvalLike: evaluates a LIKE clause.                             */
/*  Syntaxe: M like P escape C. strg->M, pat->P, C not implemented yet */
/***********************************************************************/
bool PlugEvalLike(PGLOBAL g, LPCSTR strg, LPCSTR pat, bool ci)
  {
  char *tp, *sp;
  bool  b;

  if (trace)
    htrc("LIKE: strg='%s' pattern='%s'\n", strg, pat);

  if (ci) {                        /* Case insensitive test             */
    if (strlen(pat) + strlen(strg) + 1 < MAX_STR)
      tp = g->Message;
    else if (!(tp = new char[strlen(pat) + strlen(strg) + 2])) {
      strcpy(g->Message, MSG(NEW_RETURN_NULL));
      longjmp(g->jumper[g->jump_level], OP_LIKE);
      } /* endif tp */
    
    sp = tp + strlen(pat) + 1;
    strlwr(strcpy(tp, pat));      /* Make a lower case copy of pat     */
    strlwr(strcpy(sp, strg));     /* Make a lower case copy of strg    */
  } else {                        /* Case sensitive test               */
    if (strlen(pat) < MAX_STR)    /* In most of the case for small pat */
      tp = g->Message;            /* Use this as temporary work space. */
    else if (!(tp = new char[strlen(pat) + 1])) {
      strcpy(g->Message, MSG(NEW_RETURN_NULL));
      longjmp(g->jumper[g->jump_level], OP_LIKE);
      } /* endif tp */
    
    strcpy(tp, pat);                  /* Make a copy to be worked into */
    sp = (char*)strg;
  } /* endif ci */

  b = EvalLikePattern(sp, tp);

  if (tp != g->Message)               /* If working space was obtained */
    delete [] tp;                     /* by the use of new, delete it. */

  return (b);
  } /* end of PlugEvalLike */

/***********************************************************************/
/*  M and P are variable length character string. If M and P are zero  */
/*  length strings then the Like predicate is true.                    */
/*                                                                     */
/*  The Like predicate is true if:                                     */
/*                                                                     */
/*  1- A subtring of M is a sequence of 0 or more contiguous <CR> of M */
/*     and each <CR> of M is part of exactly one substring.            */
/*                                                                     */
/*  2- If the i-th <subtring-specifyer> of P is an <arbitrary-char-    */
/*     specifier>, the i-th subtring of M is any single <CR>.          */
/*                                                                     */
/*  3- If the i-th <subtring-specifyer> of P is an <arbitrary-string-  */
/*     specifier>, then the i-th subtring of M is any sequence of zero */
/*     or more <CR>.                                                   */
/*                                                                     */
/*  4- If the i-th <subtring-specifyer> of P is neither an <arbitrary- */
/*     character-specifier> nor an <arbitrary-string-specifier>, then  */
/*     the i-th substring of M is equal to that <substring-specifier>  */
/*     according to the collating sequence of the <like-predicate>,    */
/*     without the appending of <space-character>, and has the same    */
/*     length as that <substring-specifier>.                           */
/*                                                                     */
/*  5- The number of substrings of M is equal to the number of         */
/*     <subtring-specifiers> of P.                                     */
/*                                                                     */
/*  Otherwise M like P is false.                                       */
/***********************************************************************/
bool EvalLikePattern(LPCSTR sp, LPCSTR tp)
  {
  LPSTR p;
  char  c;
  int   n;
  bool  b, t = false;

  if (trace)
    htrc("Eval Like: sp=%s tp=%s\n", 
         (sp) ? sp : "Null", (tp) ? tp : "Null");

  /********************************************************************/
  /*  If pattern is void, Like is true only if string is also void.   */
  /********************************************************************/
  if (!*tp)
    return (!*sp);

  /********************************************************************/
  /*  Analyse eventual arbitrary specifications ahead of pattern.     */
  /********************************************************************/
  for (p = (LPSTR)tp; p;)
    switch (*p) {                     /*   it can contain % and/or _   */
      case '%':                       /* An % has been found           */
        t = true;                     /* Note eventual character skip  */
        p++;
        break;
      case '_':                       /* An _ has been found           */
        if (*sp) {                    /* If more character in string   */
          sp++;                       /*   skip it                     */
          p++;
        } else
          return false;               /* Like condition is not met     */

        break;
      default:
        tp = p;                       /* Point to rest of template     */
        p = NULL;                     /* To stop For loop              */
        break;
      } /* endswitch */

  if ((p = (LPSTR)strpbrk(tp, "%_"))) /* Get position of next % or _   */
    n = p - tp;
  else
    n = strlen(tp);                   /* Get length of pattern head    */

  if (trace)
    htrc(" testing: t=%d sp=%s tp=%s p=%p\n", t, sp, tp, p);

  if (n > (signed)strlen(sp))         /* If head is longer than strg   */
    b = false;                        /* Like condition is not met     */
  else if (n == 0)                    /* If void <substring-specifier> */
    b = (t || !*sp);                  /*   true if %  or void strg.    */
  else if (!t) {
    /*******************************************************************/
    /*  No character to skip, check occurence of <subtring-specifier>  */
    /*  at the very beginning of remaining string.                     */
    /*******************************************************************/
    if (p) {
      if ((b = !strncmp(sp, tp, n)))
        b = EvalLikePattern(sp + n, p);

    } else
      b = !strcmp(sp, tp);            /*   strg and tmp heads match    */

  } else
    if (p)
      /*****************************************************************/
      /*  Here is the case explaining why we need a recursive routine. */
      /*  The test must be done not only against the first occurence   */
      /*  of the <substring-specifier> in the remaining string,        */
      /*  but also with all eventual succeeding ones.                  */
      /*****************************************************************/
      for (b = false, c = *p; !b && (signed)strlen(sp) >= n; sp++) {
        *p = '\0';                    /* Separate pattern header       */

        if ((sp = strstr(sp, tp))) {
          *p = c;
          b = EvalLikePattern(sp + n, p);
        } else {
          *p = c;
          b = false;
          break;
        } /* endif s */

        } /* endfor b, sp */

    else {
      sp += (strlen(sp) - n);
      b = !strcmp(sp, tp);
    } /* endif p */

  if (trace)
    htrc(" done: b=%d n=%d sp=%s tp=%s\n",
          b, n, (sp) ? sp : "Null", tp);

  return (b);
  } /* end of EvalLikePattern */

/***********************************************************************/
/*  MakeEscape: Escape some characters in a string.                    */
/***********************************************************************/
char *MakeEscape(PGLOBAL g, char* str, char q)
  {
  char *bufp;
  int i, k, n = 0, len = (int)strlen(str);

  for (i = 0; i < len; i++)
    if (str[i] == q || str[i] == '\\')
      n++;

  if (!n)
    return str;
  else
    bufp = (char*)PlugSubAlloc(g, NULL, len + n + 1);

  for (i = k = 0; i < len; i++) {
    if (str[i] == q || str[i] == '\\')
      bufp[k++] = '\\';

    bufp[k++] = str[i];
    } // endfor i

  bufp[k] = 0;
  return bufp;
  } /* end of MakeEscape */

/***********************************************************************/
/*  PlugConvertConstant: convert a Plug constant to an Xobject.        */
/***********************************************************************/
void PlugConvertConstant(PGLOBAL g, void* & value, short& type)
  {
  if (trace)
    htrc("PlugConvertConstant: value=%p type=%hd\n", value, type);

  if (type != TYPE_XOBJECT) {
    value = new(g) CONSTANT(g, value, type);
    type = TYPE_XOBJECT;
    } // endif type

  } // end of PlugConvertConstant

/***********************************************************************/
/*  Call the Flex preparser to convert a date format to a sscanf input */
/*  format and a Strftime output format. Flag if not 0 indicates that  */
/*  non quoted blanks are not included in the output format.           */
/***********************************************************************/
PDTP MakeDateFormat(PGLOBAL g, PSZ dfmt, bool in, bool out, int flag)
{
	int  rc;
  PDTP pdp = (PDTP)PlugSubAlloc(g, NULL, sizeof(DATPAR));

  if (trace)
    htrc("MakeDateFormat: dfmt=%s\n", dfmt);

  memset(pdp, 0, sizeof(DATPAR));
  pdp->Format = pdp->Curp = dfmt;
  pdp->Outsize = 2 * strlen(dfmt) + 1;

  if (in)
    pdp->InFmt = (char*)PlugSubAlloc(g, NULL, pdp->Outsize);

  if (out)
    pdp->OutFmt = (char*)PlugSubAlloc(g, NULL, pdp->Outsize);

  pdp->Flag = flag;

  /*********************************************************************/
  /* Call the FLEX generated parser. In multi-threading mode the next  */
  /* instruction is included in an Enter/LeaveCriticalSection bracket. */
  /*********************************************************************/
#if defined(THREAD)
#if defined(__WIN__)
  EnterCriticalSection((LPCRITICAL_SECTION)&parsec);
#else   // !__WIN__
  pthread_mutex_lock(&parmut);
#endif  // !__WIN__
#endif  //  THREAD
  rc = fmdflex(pdp);
#if defined(THREAD)
#if defined(__WIN__)
  LeaveCriticalSection((LPCRITICAL_SECTION)&parsec);
#else   // !__WIN__
  pthread_mutex_unlock(&parmut);
#endif  // !__WIN__
#endif  //  THREAD

  if (trace)
    htrc("Done: in=%s out=%s rc=%d\n", SVP(pdp->InFmt), SVP(pdp->OutFmt), rc);

  return pdp;
} // end of MakeDateFormat

/***********************************************************************/
/* Extract the date from a formatted string according to format.       */
/***********************************************************************/
int ExtractDate(char *dts, PDTP pdp, int defy, int val[6])
  {
  char *fmt, c, d, e, W[8][12];
  int   i, k, m, numval;
  int   n, y = 30;
  bool  b = true;           // true for null dates

  if (pdp)
    fmt = pdp->InFmt;
  else            // assume standard MySQL date format
    fmt = "%4d-%2d-%2d %2d:%2d:%2d";

  if (trace > 1)
    htrc("ExtractDate: dts=%s fmt=%s defy=%d\n", dts, fmt, defy);

  // Set default values for time only use
  if (defy) {
    // This may be a default value for year
    y = defy;
    val[0] = y;
    y = (y < 100) ? y : 30;
  } else
    val[0] = 70;

  val[1] = 1;
  val[2] = 1;

  for (i = 3; i < 6; i++)
    val[i] = 0;

  numval = 0;

  // Get the date field parse it with derived input format
  m = sscanf(dts, fmt, W[0], W[1], W[2], W[3], W[4], W[5], W[6], W[7]);

  if (m > pdp->Num)
    m = pdp->Num;

  for (i = 0; i < m; i++) {
    if ((n = *(int*)W[i]))
      b = false;

    switch (k = pdp->Index[i]) {
      case 0:
        if (n < y)
          n += 100;

        val[0] = n;
        numval = MY_MAX(numval, 1);
        break;
      case 1:
      case 2:
      case 3:
      case 4:
      case 5:
        val[k] = n;
        numval = MY_MAX(numval, k + 1);
        break;
      case -1:
        c = toupper(W[i][0]);
        d = toupper(W[i][1]);
        e = toupper(W[i][2]);

        switch (c) {
          case 'J':
            n = (d == 'A') ? 1
              : (e == 'N') ? 6 : 7; break;
          case 'F': n =  2; break;
          case 'M':
            n = (e == 'R') ? 3 : 5; break;
          case 'A':
            n = (d == 'P') ? 4 : 8; break;
            break;
          case 'S': n =  9; break;
          case 'O': n = 10; break;
          case 'N': n = 11; break;
          case 'D': n = 12; break;
          } /* endswitch c */

        val[1] = n;
        numval = MY_MAX(numval, 2);
        break;
      case -6:
        c = toupper(W[i][0]);
        n = val[3] % 12;

        if (c == 'P')
          n += 12;

        val[3] = n;
        break;
      } // endswitch Plugpar

    } // endfor i

  if (trace > 1)
    htrc("numval=%d val=(%d,%d,%d,%d,%d,%d)\n",
          numval, val[0], val[1], val[2], val[3], val[4], val[5]); 

  return (b) ? 0 : numval;
  } // end of ExtractDate

/***********************************************************************/
/*  Open file routine: the purpose of this routine is to make a list   */
/*  of all open file so they can be closed in SQLINIT on error jump.   */
/***********************************************************************/
FILE *PlugOpenFile(PGLOBAL g, LPCSTR fname, LPCSTR ftype)
  {
  FILE     *fop;
  PFBLOCK   fp;
  PDBUSER   dbuserp = (PDBUSER)g->Activityp->Aptr;

  if (trace) {
    htrc("PlugOpenFile: fname=%s ftype=%s\n", fname, ftype);
    htrc("dbuserp=%p\n", dbuserp);
    } // endif trace

  if ((fop= global_fopen(g, MSGID_OPEN_MODE_STRERROR, fname, ftype)) != NULL) {
    if (trace)
      htrc(" fop=%p\n", fop);

    fp = (PFBLOCK)PlugSubAlloc(g, NULL, sizeof(FBLOCK));

    if (trace)
      htrc(" fp=%p\n", fp);

    // fname may be in volatile memory such as stack
    fp->Fname = PlugDup(g, fname);
    fp->Count = 1;
    fp->Type = TYPE_FB_FILE;
    fp->File = fop;
    fp->Mode = MODE_ANY;                        // ???
    fp->Next = dbuserp->Openlist;
    dbuserp->Openlist = fp;
    } /* endif fop */

  if (trace)
    htrc(" returning fop=%p\n", fop);

  return (fop);
  } // end of PlugOpenFile

/***********************************************************************/
/*  Close file routine: the purpose of this routine is to avoid        */
/*  double closing that freeze the system on some Unix platforms.      */
/***********************************************************************/
FILE *PlugReopenFile(PGLOBAL g, PFBLOCK fp, LPCSTR md)
  {
  FILE *fop;

  if ((fop = global_fopen(g, MSGID_OPEN_MODE_STRERROR, fp->Fname, md))) {
    fp->Count = 1;
    fp->Type = TYPE_FB_FILE;
    fp->File = fop;
    } /* endif fop */

  return (fop);
  } // end of PlugOpenFile

/***********************************************************************/
/*  Close file routine: the purpose of this routine is to avoid        */
/*  double closing that freeze the system on some Unix platforms.      */
/***********************************************************************/
int PlugCloseFile(PGLOBAL g __attribute__((unused)), PFBLOCK fp, bool all)
  {
  int rc = 0;

  if (trace)
    htrc("PlugCloseFile: fp=%p count=%hd type=%hd\n",
          fp, ((fp) ? fp->Count : 0), ((fp) ? fp->Type : 0));

  if (!fp || !fp->Count)
    return rc;

  switch (fp->Type) {
    case TYPE_FB_FILE:
      if (fclose((FILE *)fp->File) == EOF)
        rc = errno;

      fp->File = NULL;
      fp->Mode = MODE_ANY;
      fp->Count = 0;
      break;
    case TYPE_FB_MAP:
      if ((fp->Count = (all) ? 0 : fp->Count - 1))
        break;

      if (CloseMemMap(fp->Memory, fp->Length))
        rc = (int)GetLastError();

      fp->Memory = NULL;
      fp->Mode = MODE_ANY;
      // Passthru
    case TYPE_FB_HANDLE:
      if (fp->Handle && fp->Handle != INVALID_HANDLE_VALUE)
        if (CloseFileHandle(fp->Handle))
          rc = (rc) ? rc : (int)GetLastError();

      fp->Handle = INVALID_HANDLE_VALUE;
      fp->Mode = MODE_ANY;
      fp->Count = 0;
      break;
#ifdef DOMDOC_SUPPORT
    case TYPE_FB_XML:
      CloseXMLFile(g, fp, all);
      break;
#endif   // DOMDOC_SUPPORT
#ifdef LIBXML2_SUPPORT
    case TYPE_FB_XML2:
      CloseXML2File(g, fp, all);
      break;
#endif   // LIBXML2_SUPPORT
    default:
      rc = RC_FX;
    } // endswitch Type

  return rc;
  } // end of PlugCloseFile

/***********************************************************************/
/*  PlugCleanup: Cleanup remaining items of a SQL query.               */
/***********************************************************************/
void PlugCleanup(PGLOBAL g, bool dofree)
  {
  PCATLG  cat;
  PDBUSER dbuserp = (PDBUSER)g->Activityp->Aptr;

  // The test on Catalog is to avoid a Windows bug that can make
  // LoadString in PlugGetMessage to fail in some case
  if (!dbuserp || !(cat = dbuserp->Catalog))
    return;

  /*********************************************************************/
  /*  Close eventually still open/mapped files.                        */
  /*********************************************************************/
  for (PFBLOCK fp = dbuserp->Openlist; fp; fp = fp->Next)
    PlugCloseFile(g, fp, true);

  dbuserp->Openlist = NULL;

  if (dofree) {
    /*******************************************************************/
    /*  Cleanup any non suballocated memory still not freed.           */
    /*******************************************************************/
    for (PMBLOCK mp = dbuserp->Memlist; mp; mp = mp->Next)
      PlgDBfree(*mp);

    dbuserp->Memlist = NULL;

    /*******************************************************************/
    /*  If not using permanent storage catalog, reset volatile values. */
    /*******************************************************************/
    cat->Reset();

    /*******************************************************************/
    /*  This is the place to reset the pointer on domains.             */
    /*******************************************************************/
    dbuserp->Subcor = false;
    dbuserp->Step = "New query";     // was STEP(PARSING_QUERY);
    dbuserp->ProgMax = dbuserp->ProgCur = dbuserp->ProgSav = 0;
    } // endif dofree

  } // end of PlugCleanup

#if 0
/***********************************************************************/
/*  That stupid Windows 98 does not provide this function.             */
/***********************************************************************/
bool WritePrivateProfileInt(LPCSTR sec, LPCSTR key, int n, LPCSTR ini)
  {
  char buf[12];

  sprintf(buf, "%d", n);
  return WritePrivateProfileString(sec, key, buf, ini);
  } // end of WritePrivateProfileInt

/***********************************************************************/
/*  Retrieve a size from an INI file with eventual K or M following.   */
/***********************************************************************/
int GetIniSize(char *section, char *key, char *def, char *ini)
  {
  char c, buff[32];
  int  i;
  int  n = 0;

  GetPrivateProfileString(section, key, def, buff, sizeof(buff), ini);

  if ((i = sscanf(buff, " %d %c ", &n, &c)) == 2)
    switch (toupper(c)) {
      case 'M':
        n *= 1024;
      case 'K':
        n *= 1024;
      } // endswitch c

  if (trace)
    htrc("GetIniSize: key=%s buff=%s i=%d n=%d\n", key, buff, i, n);

  return n;
  } // end of GetIniSize

/***********************************************************************/
/* Allocate a string retrieved from an INI file and return its address */
/***********************************************************************/
DllExport PSZ GetIniString(PGLOBAL g, void *mp, LPCSTR sec, LPCSTR key,
                                                LPCSTR def, LPCSTR ini)
  {
  char  buff[_MAX_PATH];
  PSZ   p;
  int   n, m = sizeof(buff);
  char *buf = buff;

#if defined(_DEBUG)
  assert (sec && key);
#endif

 again:
  n = GetPrivateProfileString(sec, key, def, buf, m, ini);

  if (n == m - 1) {
    // String may have been truncated, make sure to have all
    if (buf != buff)
      delete [] buf;

    m *= 2;
    buf = new char[m];
    goto again;
    } // endif n

  p = (PSZ)PlugSubAlloc(g, mp, n + 1);

  if (trace)
    htrc("GetIniString: sec=%s key=%s buf=%s\n", sec, key, buf);

  strcpy(p, buf);

  if (buf != buff)
    delete [] buf;

  return p;
  } // end of GetIniString
#endif // 0

/***********************************************************************/
/*  GetAmName: return the name correponding to an AM code.             */
/***********************************************************************/
char *GetAmName(PGLOBAL g, AMT am, void *memp)
  {
  char *amn= (char*)PlugSubAlloc(g, memp, 16);

  switch (am) {
    case TYPE_AM_ERROR: strcpy(amn, "ERROR"); break;
    case TYPE_AM_ROWID: strcpy(amn, "ROWID"); break;
    case TYPE_AM_FILID: strcpy(amn, "FILID"); break;
    case TYPE_AM_VIEW:  strcpy(amn, "VIEW");  break;
    case TYPE_AM_COUNT: strcpy(amn, "COUNT"); break;
    case TYPE_AM_DCD:   strcpy(amn, "DCD");   break;
    case TYPE_AM_CMS:   strcpy(amn, "CMS");   break;
    case TYPE_AM_MAP:   strcpy(amn, "MAP");   break;
    case TYPE_AM_FMT:   strcpy(amn, "FMT");   break;
    case TYPE_AM_CSV:   strcpy(amn, "CSV");   break;
    case TYPE_AM_MCV:   strcpy(amn, "MCV");   break;
    case TYPE_AM_DOS:   strcpy(amn, "DOS");   break;
    case TYPE_AM_FIX:   strcpy(amn, "FIX");   break;
    case TYPE_AM_BIN:   strcpy(amn, "BIN");   break;
    case TYPE_AM_VCT:   strcpy(amn, "VEC");   break;
    case TYPE_AM_VMP:   strcpy(amn, "VMP");   break;
    case TYPE_AM_DBF:   strcpy(amn, "DBF");   break;
    case TYPE_AM_QRY:   strcpy(amn, "QRY");   break;
    case TYPE_AM_SQL:   strcpy(amn, "SQL");   break;
    case TYPE_AM_PLG:   strcpy(amn, "PLG");   break;
    case TYPE_AM_PLM:   strcpy(amn, "PLM");   break;
    case TYPE_AM_DOM:   strcpy(amn, "DOM");   break;
    case TYPE_AM_DIR:   strcpy(amn, "DIR");   break;
    case TYPE_AM_ODBC:  strcpy(amn, "ODBC");  break;
    case TYPE_AM_MAC:   strcpy(amn, "MAC");   break;
    case TYPE_AM_OEM:   strcpy(amn, "OEM");   break;
    case TYPE_AM_OUT:   strcpy(amn, "OUT");   break;
    default:           sprintf(amn, "OEM(%d)", am);
    } // endswitch am

  return amn;
  } // end of GetAmName

#if defined(__WIN__) && !defined(NOCATCH)
/***********************************************************************/
/*  GetExceptionDesc: return the description of an exception code.     */
/***********************************************************************/
char *GetExceptionDesc(PGLOBAL g, unsigned int e)
  {
  char *p;

  switch (e) {
    case EXCEPTION_GUARD_PAGE:
      p = MSG(GUARD_PAGE);
      break;
    case EXCEPTION_DATATYPE_MISALIGNMENT:
      p = MSG(DATA_MISALIGN);
      break;
    case EXCEPTION_BREAKPOINT:
      p = MSG(BREAKPOINT);
      break;
    case EXCEPTION_SINGLE_STEP:
      p = MSG(SINGLE_STEP);
      break;
    case EXCEPTION_ACCESS_VIOLATION:
      p = MSG(ACCESS_VIOLATN);
      break;
    case EXCEPTION_IN_PAGE_ERROR:
      p = MSG(PAGE_ERROR);
      break;
    case EXCEPTION_INVALID_HANDLE:
      p = MSG(INVALID_HANDLE);
      break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
      p = MSG(ILLEGAL_INSTR);
      break;
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
      p = MSG(NONCONT_EXCEPT);
      break;
    case EXCEPTION_INVALID_DISPOSITION:
      p = MSG(INVALID_DISP);
      break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
      p = MSG(ARRAY_BNDS_EXCD);
      break;
    case EXCEPTION_FLT_DENORMAL_OPERAND:
      p = MSG(FLT_DENORMAL_OP);
      break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
      p = MSG(FLT_ZERO_DIVIDE);
      break;
    case EXCEPTION_FLT_INEXACT_RESULT:
      p = MSG(FLT_BAD_RESULT);
      break;
    case EXCEPTION_FLT_INVALID_OPERATION:
      p = MSG(FLT_INVALID_OP);
      break;
    case EXCEPTION_FLT_OVERFLOW:
      p = MSG(FLT_OVERFLOW);
      break;
    case EXCEPTION_FLT_STACK_CHECK:
      p = MSG(FLT_STACK_CHECK);
      break;
    case EXCEPTION_FLT_UNDERFLOW:
      p = MSG(FLT_UNDERFLOW);
      break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      p = MSG(INT_ZERO_DIVIDE);
      break;
    case EXCEPTION_INT_OVERFLOW:
      p = MSG(INT_OVERFLOW);
      break;
    case EXCEPTION_PRIV_INSTRUCTION:
      p = MSG(PRIV_INSTR);
      break;
    case EXCEPTION_STACK_OVERFLOW:
      p = MSG(STACK_OVERFLOW);
      break;
    case CONTROL_C_EXIT:
      p = MSG(CONTROL_C_EXIT);
      break;
    case STATUS_NO_MEMORY:
      p = MSG(NO_MEMORY);
      break;
    default:
      p = MSG(UNKNOWN_EXCPT);
      break;
    } // endswitch nSE

  return p;
  } // end of GetExceptionDesc
#endif   // __WIN__ && !NOCATCH

/***********************************************************************/
/*  PlgDBalloc: allocates or suballocates memory conditionally.        */
/*  If mp.Sub is true at entry, this forces suballocation.             */
/*  If the memory is allocated, makes an entry in an allocation list   */
/*  so it can be freed at the normal or error query completion.        */
/***********************************************************************/
void *PlgDBalloc(PGLOBAL g, void *area, MBLOCK& mp)
  {
//bool        b;
  size_t      maxsub, minsub;
  void       *arp = (area) ? area : g->Sarea;
  PPOOLHEADER pph = (PPOOLHEADER)arp;

  if (mp.Memp) {
    // This is a reallocation. If this block is not suballocated, it
    // was already placed in the chain of memory blocks and we must
    // not do it again as it can trigger a loop when freeing them.
    // Note: this works if blocks can be reallocated only once.
    // Otherwise a new boolean must be added to the block that
    // indicate that it is chained, or a test on the whole chain be
    // done to check whether the block is already there.
//  b = mp.Sub;
    mp.Sub = false;    // Restrict suballocation to one quarter
    } // endif Memp

  // Suballoc when possible if mp.Sub is initially true, but leaving
  // a minimum amount of storage for future operations such as the
  // optimize recalculation after insert; otherwise
  // suballoc only if size is smaller than one quarter of free mem.
  minsub = (pph->FreeBlk + pph->To_Free + 524248) >> 2;
  maxsub = (pph->FreeBlk < minsub) ? 0 : pph->FreeBlk - minsub;
  mp.Sub = mp.Size <= ((mp.Sub) ? maxsub : (maxsub >> 2));

  if (trace > 1)
    htrc("PlgDBalloc: in %p size=%d used=%d free=%d sub=%d\n",
          arp, mp.Size, pph->To_Free, pph->FreeBlk, mp.Sub);

  if (!mp.Sub) {
    // For allocations greater than one fourth of remaining storage
    // in the area, do allocate from virtual storage.
#if defined(__WIN__)
    if (mp.Size >= BIGMEM)
      mp.Memp = VirtualAlloc(NULL, mp.Size, MEM_COMMIT, PAGE_READWRITE);
    else
#endif
      mp.Memp = malloc(mp.Size);

    if (!mp.Inlist && mp.Memp) {
      // New allocated block, put it in the memory block chain.
      PDBUSER dbuserp = (PDBUSER)g->Activityp->Aptr;

      mp.Next = dbuserp->Memlist;
      dbuserp->Memlist = &mp;
      mp.Inlist = true;
      } // endif mp

  } else
    // Suballocating is Ok.
    mp.Memp = PlugSubAlloc(g, area, mp.Size);

  return mp.Memp;
  } // end of PlgDBalloc

/***********************************************************************/
/*  PlgDBrealloc: reallocates memory conditionally.                    */
/*  Note that this routine can fail only when block size is increased  */
/*  because otherwise we keep the old storage on failure.              */
/***********************************************************************/
void *PlgDBrealloc(PGLOBAL g, void *area, MBLOCK& mp, size_t newsize)
  {
  MBLOCK m;

#if defined(_DEBUG)
//  assert (mp.Memp != NULL);
#endif

  if (trace > 1)
    htrc("PlgDBrealloc: %p size=%d sub=%d\n", mp.Memp, mp.Size, mp.Sub);

  if (newsize == mp.Size)
    return mp.Memp;      // Nothing to do
  else
    m = mp;

  if (!mp.Sub && mp.Size < BIGMEM && newsize < BIGMEM) {
    // Allocation was done by malloc, try to use realloc but
    // suballoc if newsize is smaller than one quarter of free mem.
    size_t      maxsub;
    PPOOLHEADER pph = (PPOOLHEADER)((area) ? area : g->Sarea);

    maxsub = (pph->FreeBlk < 131072) ? 0 : pph->FreeBlk - 131072;

    if ((mp.Sub = (newsize <= (maxsub >> 2)))) {
      mp.Memp = PlugSubAlloc(g, area, newsize);
      memcpy(mp.Memp, m.Memp, MY_MIN(m.Size, newsize));
      PlgDBfree(m);    // Free the old block
    } else if (!(mp.Memp = realloc(mp.Memp, newsize))) {
      mp = m;          // Possible only if newsize > Size
      return NULL;     // Failed
    } // endif's

    mp.Size = newsize;
  } else if (!mp.Sub || newsize > mp.Size) {
    // Was suballocated or Allocation was done by VirtualAlloc
    // Make a new allocation and copy the useful part
    // Note: DO NOT reset Memp and Sub so we know that this
    // is a reallocation in PlgDBalloc
    mp.Size = newsize;

    if (PlgDBalloc(g, area, mp)) {
      memcpy(mp.Memp, m.Memp, MY_MIN(m.Size, newsize));
      PlgDBfree(m);    // Free the old block
    } else {
      mp = m;          // No space to realloc, do nothing

      if (newsize > m.Size)
        return NULL;   // Failed

    } // endif PlgDBalloc

  } // endif's

  if (trace)
    htrc(" newsize=%d newp=%p sub=%d\n", mp.Size, mp.Memp, mp.Sub);

  return mp.Memp;
  } // end of PlgDBrealloc

/***********************************************************************/
/*  PlgDBfree: free memory if not suballocated.                        */
/***********************************************************************/
void PlgDBfree(MBLOCK& mp)
  {
  if (trace > 1)
    htrc("PlgDBfree: %p sub=%d size=%d\n", mp.Memp, mp.Sub, mp.Size);

  if (!mp.Sub && mp.Memp)
#if defined(__WIN__)
    if (mp.Size >= BIGMEM)
      VirtualFree(mp.Memp, 0, MEM_RELEASE);
    else
#endif
      free(mp.Memp);

  // Do not reset Next to avoid cutting the Mblock chain
  mp.Memp = NULL;
  mp.Sub = false;
  mp.Size = 0;
  } // end of PlgDBfree

/***********************************************************************/
/*  Program for sub-allocating one item in a storage area.             */
/*  Note: This function is equivalent to PlugSubAlloc except that in   */
/*  case of insufficient memory, it returns NULL instead of doing a    */
/*  long jump. The caller must test the return value for error.        */
/***********************************************************************/
void *PlgDBSubAlloc(PGLOBAL g, void *memp, size_t size)
  {
  PPOOLHEADER pph;                           // Points on area header.

  if (!memp)
    /*******************************************************************/
    /*  Allocation is to be done in the Sarea.                         */
    /*******************************************************************/
    memp = g->Sarea;

//size = ((size + 3) / 4) * 4;       /* Round up size to multiple of 4 */
  size = ((size + 7) / 8) * 8;       /* Round up size to multiple of 8 */
  pph = (PPOOLHEADER)memp;

  if (trace > 1)
    htrc("PlgDBSubAlloc: memp=%p size=%d used=%d free=%d\n",
         memp, size, pph->To_Free, pph->FreeBlk);

  if ((uint)size > pph->FreeBlk) {   /* Not enough memory left in pool */
    sprintf(g->Message,
    "Not enough memory in Work area for request of %d (used=%d free=%d)",
            (int) size, pph->To_Free, pph->FreeBlk);

    if (trace)
      htrc("%s\n", g->Message);

    return NULL;
    } // endif size

  /*********************************************************************/
  /*  Do the suballocation the simplest way.                           */
  /*********************************************************************/
  memp = MakePtr(memp, pph->To_Free);   // Points to suballocated block
  pph->To_Free += size;                 // New offset of pool free block
  pph->FreeBlk -= size;                 // New size   of pool free block

  if (trace > 1)
    htrc("Done memp=%p used=%d free=%d\n",
         memp, pph->To_Free, pph->FreeBlk);

  return (memp);
  } // end of PlgDBSubAlloc

/***********************************************************************/
/*  Program for sub-allocating and copying a string in a storage area. */
/***********************************************************************/
char *PlgDBDup(PGLOBAL g, const char *str)
  {
  if (str) {
    char *sm = (char*)PlgDBSubAlloc(g, NULL, strlen(str) + 1);

    if (sm)
      strcpy(sm, str);

    return sm;
  } else
    return NULL;

  } // end of PlgDBDup

/***********************************************************************/
/*  PUTOUT: Plug DB object typing routine.                             */
/***********************************************************************/
void PlugPutOut(PGLOBAL g, FILE *f, short t, void *v, uint n)
  {
  char  m[64];

  if (trace)
    htrc("PUTOUT: f=%p t=%d v=%p n=%d\n", f, t, v, n);

  if (!v)
    return;

  memset(m, ' ', n);                             /* Make margin string */
  m[n] = '\0';
  n += 2;                                        /* Increase margin    */

  switch (t) {
    case TYPE_ERROR:
      fprintf(f, "--> %s\n", (PSZ)v);
      break;

    case TYPE_STRING:
    case TYPE_PSZ:
      fprintf(f, "%s%s\n", m, (PSZ)v);
      break;

    case TYPE_DOUBLE:
      fprintf(f, "%s%lf\n", m, *(double *)v);
      break;

    case TYPE_LIST:
    case TYPE_COLIST:
    case TYPE_COL:
     {PPARM p;

      if (t == TYPE_LIST)
        fprintf(f, "%s%s\n", m, MSG(LIST));
      else
        fprintf(f, "%s%s\n", m, "Colist:");

      for (p = (PPARM)v; p; p = p->Next)
        PlugPutOut(g, f, p->Type, p->Value, n);

      } break;

    case TYPE_INT:
      fprintf(f, "%s%d\n", m, *(int *)v);
      break;

    case TYPE_SHORT:
      fprintf(f, "%s%hd\n", m, *(short *)v);
      break;

    case TYPE_TINY:
      fprintf(f, "%s%d\n", m, (int)*(char *)v);
      break;

    case TYPE_VOID:
      break;

    case TYPE_SQL:
    case TYPE_TABLE:
    case TYPE_TDB:
    case TYPE_XOBJECT:
      ((PBLOCK)v)->Print(g, f, n-2);
      break;

    default:
      fprintf(f, "%s%s %d\n", m, MSG(ANSWER_TYPE), t);
    } /* endswitch */

  return;
  } /* end of PlugPutOut */

/***********************************************************************/
/*  NewPointer: makes a table of pointer values to be changed later.   */
/***********************************************************************/
DllExport void NewPointer(PTABS t, void *oldv, void *newv)
  {
  PTABPTR tp;

  if (!oldv)                                       /* error ?????????? */
    return;

  if (!t->P1 || t->P1->Num == 50)
  {
    if (!(tp = new TABPTR)) {
      PGLOBAL g = t->G;

      sprintf(g->Message, "NewPointer: %s", MSG(MEM_ALLOC_ERROR));
      longjmp(g->jumper[g->jump_level], 3);
    } else {
      tp->Next = t->P1;
      tp->Num = 0;
      t->P1 = tp;
    } /* endif tp */
  }

  t->P1->Old[t->P1->Num] = oldv;
  t->P1->New[t->P1->Num++] = newv;
  } /* end of NewPointer */

#if 0
/***********************************************************************/
/*  Compare two files and return 0 if they are identical, else 1.      */
/***********************************************************************/
int FileComp(PGLOBAL g, char *file1, char *file2)
  {
  char *fn[2], *bp[2], buff1[4096], buff2[4096];
  int   i, k, n[2], h[2] = {-1,-1};
  int  len[2], rc = -1;

  fn[0] = file1; fn[1] = file2;
  bp[0] = buff1; bp[1] = buff2;

  for (i = 0; i < 2; i++) {
#if defined(__WIN__)
    h[i]= global_open(g, MSGID_NONE, fn[i], _O_RDONLY | _O_BINARY);
#else   // !__WIN__
    h[i]= global_open(g, MSGOD_NONE, fn[i], O_RDONLY);
#endif  // !__WIN__

    if (h[i] == -1) {
//      if (errno != ENOENT) {
        sprintf(g->Message, MSG(OPEN_MODE_ERROR),
                "rb", (int)errno, fn[i]);
        strcat(strcat(g->Message, ": "), strerror(errno));
        longjmp(g->jumper[g->jump_level], 666);
//      } else
//        len[i] = 0;          // File does not exist yet

    } else {
      if ((len[i] = _filelength(h[i])) < 0) {
        sprintf(g->Message, MSG(FILELEN_ERROR), "_filelength", fn[i]);
        longjmp(g->jumper[g->jump_level], 666);
        } // endif len

    } // endif h

    } // endfor i

  if (len[0] != len[1])
    rc = 1;

  while (rc == -1) {
    for (i = 0; i < 2; i++)
      if ((n[i] = read(h[i], bp[i], 4096)) < 0) {
        sprintf(g->Message, MSG(READ_ERROR), fn[i], strerror(errno));
        goto fin;
        } // endif n

    if (n[0] != n[1])
      rc = 1;
    else if (*n == 0)
      rc = 0;
    else for (k = 0; k < *n; k++)
      if (*(bp[0] + k) != *(bp[1] + k)) {
        rc = 1;
        goto fin;
        } // endif bp

    } // endwhile

 fin:
  for (i = 0; i < 2; i++)
    if (h[i] != -1)
      close(h[i]);

  return rc;
  } // end of FileComp
#endif // 0
