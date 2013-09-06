/***********************************************************************/
/*  PLGODBC.H - This is the ODBC PlugDB driver include file.           */
/***********************************************************************/
//efine WINVER 0x0300 // prevent Windows 3.1 feature usage
#include <windows.h>                           /* Windows include file */
#include <windowsx.h>                          /* Message crackers     */
#include <commctrl.h>

/***********************************************************************/
/*  Included C-definition files required by the interface.             */
/***********************************************************************/
#include <string.h>                  /* String manipulation declares   */
#include <stdlib.h>                  /* C standard library             */
#include <ctype.h>                   /* C language specific types      */
#include <stdio.h>                   /* FOPEN_MAX   declaration        */
#include <time.h>                    /* time_t type declaration        */

/***********************************************************************/
/*  ODBC interface of PlugDB driver declares.                          */
/***********************************************************************/
#include "podbcerr.h"                /* Resource ID for PlugDB Driver  */
#if !defined(WIN32)
#include "w16macro.h"
#endif

#define ODBCVER    0x0300

#include "sqltypes.h"
#include "sql.h"
#include "sqlext.h"

/***********************************************************************/
/*  Definitions to be used in function prototypes.                     */
/*  The SQL_API is to be used only for those functions exported for    */
/*    driver manager use.                                              */
/*  The EXPFUNC is to be used only for those functions exported but    */
/*    used internally, ie, dialog procs.                               */
/*  The INTFUNC is to be used for all other functions.                 */
/***********************************************************************/
#if defined(WIN32)
#define INTFUNC  __stdcall
#define EXPFUNC  __stdcall
#else
#define INTFUNC FAR PASCAL
#define EXPFUNC __export CALLBACK
#endif

/***********************************************************************/
/*  External variables.                                                */
/***********************************************************************/
extern HINSTANCE NEAR s_hModule;  // DLL handle.
#ifdef DEBTRACE
extern FILE *debug;
#endif
extern bool clearerror;

/***********************************************************************/
/*  Additional values used by PlugDB for ODBC.                         */
/***********************************************************************/
#define RES_TYPE_PREPARE       1        /* Result from SQLPrepare      */
#define RES_TYPE_CATALOG       2        /* Result from catalog funcs   */
#define MAXPATHLEN     _MAX_PATH        /* Max path length             */
#define MAXKEYLEN             16        /* Max keyword length          */
#define MAXDESC              256        /* Max description length      */
#define MAXDSNAME             33        /* Max data source name length */
#define MAXCRNAME             18        /* Max stmt cursor name length */
#define DEFMAXRES           6300        /* Default MaxRes value        */
#define NAM_LEN              128        /* Length of col and tab names */

#define MAXRESULT           1000        /* ? */
#define MAXCOMMAND           200        /* ? */
#define RC_ERROR       RC_LAST-1
#define RC_FREE                3

#if !defined(NOLIB)
#define CNXKEY         uint             /* C Key returned by Conn DLL  */
#else
typedef struct _conninfo *PCONN;
#endif   /* !NOLIB */

#if defined(DEBTRACE)
#define TRACE0(X)              fprintf(debug,X);
#define TRACE1(X,A)            fprintf(debug,X,A);
#define TRACE2(X,A,B)          fprintf(debug,X,A,B);
#define TRACE3(X,A,B,C)        fprintf(debug,X,A,B,C);
#define TRACE4(X,A,B,C,D)      fprintf(debug,X,A,B,C,D);
#define TRACE5(X,A,B,C,D,E)    fprintf(debug,X,A,B,C,D,E);
#define TRACE6(X,A,B,C,D,E,F)  fprintf(debug,X,A,B,C,D,E,F);
#else    /* !DEBTRACE*/
#define TRACE0(X)          
#define TRACE1(X,A)        
#define TRACE2(X,A,B)      
#define TRACE3(X,A,B,C)    
#define TRACE4(X,A,B,C,D)  
#define TRACE5(X,A,B,C,D,E)
#define TRACE6(X,A,B,C,D,E,F)
#endif  /* !DEBTRACE*/

// This definition MUST be identical to the value in plgdbsem.h
#define XMOD_PREPARE  1

/***********************************************************************/
/*  ODBC.INI keywords (use extern definition in SETUP.C)               */
/***********************************************************************/
extern char const *EMPTYSTR;              /* Empty String              */
extern char const *OPTIONON;
extern char const *OPTIONOFF;
extern char const *INI_SDEFAULT;          /* Default data source name  */
extern char const *ODBC_INI;              /* ODBC initialization file  */
extern char const *INI_KDEFL;             /* Is SQL to use by default? */
extern char const *INI_KLANG;             /* Application language      */
extern char const *INI_KDATA;             /* Data description file     */
extern char const *INI_KSVR;              /* PLG Server                */

/************************************************************************/
/*  Attribute key indexes (into an array of Attr structs, see below)    */
/************************************************************************/
#define KEY_DSN         0
#define KEY_DEFL        1
#define KEY_LANG        2
#define KEY_DATA        3
#define KEY_SERVER      4
#define LAST_KEY        5                 /* Number of keys in TAG's   */
#define KEY_DESC        5
#define KEY_TRANSNAME   6
#define KEY_TRANSOPTION 7
#define KEY_TRANSDLL    8
#define NUMOFKEYS       9                 /* Number of keys supported   */

#define FOURYEARS    126230400    // Four years in seconds (1 leap)

/***********************************************************************/
/*  This is used when an "out of memory" error happens, because this   */
/*  error recording system allocates memory when it logs an error,     */
/*  and it would be bad if it tried to allocate memory when it got an  */
/*  out of memory error.                                               */
/***********************************************************************/
typedef enum _ERRSTAT {
  errstatOK,
  errstatNO_MEMORY,
  } ERRSTAT;

/***********************************************************************/
/*  Types                                                              */
/***********************************************************************/
typedef struct TagAttr {
  bool fSupplied;
  char Attr[MAXPATHLEN];
  } TAG, *PTAG;

typedef struct _parscons {              /* Parse constants             */
  int  Slen;                            /* String length               */
  int  Ntag;                            /* Number of entries in tags   */
  int  Nlook;                           /* Number of entries in lookup */
  char Sep;                             /* Separator                   */
  } PARC, *PPARC;

/***********************************************************************/
/*  Attribute string look-up table (maps keys to associated indexes)   */
/***********************************************************************/
typedef struct _Look {
  const char *szKey;
  int         iKey;
  } LOOK, *PLOOK;

/***********************************************************************/
/*  This is info about a single error.                                 */
/***********************************************************************/
typedef struct _ERRBLK  *PERRBLK;

typedef struct _ERRBLK {
  PERRBLK Next;           /* Next block in linked list of error blocks */
  DWORD   Native_Error;                                /* Native error */
  DWORD   Stderr;                                   /* SQLC error code */
  PSZ     Message;                        /* Points to text of message */
  } ERRBLK;

/***********************************************************************/
/*  This is a header block, it records information about a list of     */
/*  errors (ERRBLOCK's).                                               */
/***********************************************************************/
typedef struct _ERRINFO {
  PERRBLK First;        /* First block in linked list of error blocks. */
  PERRBLK Last;         /* Last block in above list.                   */
  ERRSTAT Errstat;      /* Status for special condition out of memory  */
  } ERRINFO, *PERRINFO;

/***********************************************************************/
/*  Environment information.                                           */
/***********************************************************************/
typedef struct _env {
  ERRINFO Errinfo;                                       /* Error list */
  UDWORD ODBCver;
  UDWORD ODBCdateformat;
  } ENV,  *PENV;

/***********************************************************************/
/*  Classes used in the PlugDB ODBC Driver.                            */
/***********************************************************************/
typedef class DBC        *PDBC;
typedef class STMT       *PSTMT;
typedef class CURSOR     *PCURSOR;
typedef class RESULT     *PRESULT;
typedef class BINDDATA   *PBIND;
typedef class BINDPARM   *PBDPARM;
typedef class CPLGdrv    *PSCDRV;
typedef class DESCRIPTOR *PDSC;

/***********************************************************************/
/*  ODBC Prototypes.                                                   */
/***********************************************************************/
void    PostSQLError(HENV, HDBC, HSTMT, DWORD, DWORD, PSZ);
void    ClearSQLError(HENV, HDBC, HSTMT);
short   LoadRcString(UWORD, LPSTR, short);
RETCODE RetcodeCopyBytes(HDBC, HSTMT, UCHAR FAR *, SWORD,
                         SWORD FAR *, UCHAR FAR *, SWORD, bool);

/***********************************************************************/
/*  Private functions used by the driver.                              */
/***********************************************************************/
bool    EXPFUNC FDriverConnectProc(HWND, WORD, WPARAM, LPARAM);
extern  void    ParseAttrString(PLOOK, PTAG, UCHAR FAR *, PPARC);
RETCODE PASCAL  StringCopy(HDBC, HSTMT, PTR, SWORD, SWORD FAR *,
                                                     char FAR *);
RETCODE PASCAL  ShortCopy(HDBC, HSTMT, PTR, SWORD, SWORD FAR *, short);
RETCODE PASCAL  LongCopy(HDBC, HSTMT, PTR, SWORD, SWORD FAR *, int);
RETCODE PASCAL  GeneralCopy(HDBC, HSTMT, PTR, SWORD,
                            SWORD FAR *, PTR, SWORD);

/* --------------------- End of PLGODBC.H ---------------------------- */
