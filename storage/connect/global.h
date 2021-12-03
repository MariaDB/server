/***********************************************************************/
/*  GLOBAL.H: Declaration file used by all CONNECT implementations.    */
/*  (C) Copyright MariaDB Corporation Ab                 							 */
/*  Author Olivier Bertrand                              1993-2020     */
/***********************************************************************/

/***********************************************************************/
/*  Included C-definition files common to all Plug routines            */
/***********************************************************************/
#include <string.h>                 /* String manipulation declares    */
#include <stdlib.h>                 /* C standard library              */
#include <ctype.h>                  /* C language specific types       */
#include <stdio.h>                  /* FOPEN_MAX   declaration         */
#include <time.h>                   /* time_t type declaration         */
#include <setjmp.h>                 /* Long jump   declarations        */

#if defined(_WIN32) && !defined(NOEX)
#define DllExport  __declspec( dllexport )
#else   // !_WIN32
#define DllExport
#endif  // !_WIN32

#if defined(DOMDOC_SUPPORT) || defined(LIBXML2_SUPPORT)
#define XML_SUPPORT 1
#endif

#if defined(XMSG)
//#error Option XMSG is not yet fully implemented
// Definition used to read messages from message file.
#include "msgid.h"
#define MSG(I)   PlugReadMessage(NULL, MSG_##I, #I)
#define STEP(I)  PlugReadMessage(g, MSG_##I, #I)
#elif defined(NEWMSG)
//#error Option NEWMSG is not yet fully implemented
// Definition used to get messages from resource.
#include "msgid.h"
#define MSG(I)   PlugGetMessage(NULL, MSG_##I)
#define STEP(I)  PlugGetMessage(g, MSG_##I)
#else   // !XMSG and !NEWMSG
// Definition used to replace messages ID's by their definition.
#include "messages.h"
#define MSG(I)                     MSG_##I
#define STEP(I)                    MSG_##I
#endif  // !XMSG and !NEWMSG

#if defined(_WIN32)
#define CRLF  2
#else    // !_WIN32
#define CRLF  1
#endif  // !_WIN32

/***********************************************************************/
/*  Define access to the thread based trace value.                     */
/***********************************************************************/
#define trace(T)  (bool)(GetTraceValue() & (uint)T)

/***********************************************************************/
/*  Miscellaneous Constants                                            */
/***********************************************************************/
#define  NO_IVAL   -95684275        /* Used by GetIntegerOption        */
#define  MAX_JUMP         24        /* Maximum jump level number       */
#define  MAX_STR        4160        /* Maximum message length          */

#define  TYPE_VOID        -1
#define  TYPE_ERROR        0
#define  TYPE_STRING       1
#define  TYPE_DOUBLE       2
#define  TYPE_SHORT        3
#define  TYPE_TINY         4
#define  TYPE_BIGINT       5
#define  TYPE_LIST         6
#define  TYPE_INT          7
#define  TYPE_DATE         8
#define  TYPE_DECIM        9
#define  TYPE_BIN         10
#define  TYPE_PCHAR       11

#if defined(__cplusplus)
extern "C" {
#endif

/***********************************************************************/
/*  Static variables                                                   */
/***********************************************************************/

/***********************************************************************/
/*                       File-Selection Indicators                     */
/***********************************************************************/
#define PAT_LOG       "log"

#if defined(UNIX) || defined(LINUX) || defined(UNIV_LINUX)
  // printf does not accept null pointer for %s target
  #define SVP(S)  ((S) ? S : "<null>")
#else
  //  printf accepts null pointer for %s target
  #define SVP(S)  S
#endif

#if defined(STORAGE)
  FILE        *debug;
#else
  extern FILE *debug;
#endif


/***********************************************************************/
/*  General purpose type definitions.                                  */
/***********************************************************************/
#include "os.h"

typedef struct {
  ushort Length;
  char   String[2];
  } VARSTR;

#if !defined(PGLOBAL_DEFINED)
typedef struct _global   *PGLOBAL;
#define PGLOBAL_DEFINED
#endif
typedef struct _globplg  *PGS;
typedef struct _activity *PACTIVITY;
typedef struct _parm     *PPARM;
typedef char   NAME[9];

/***********************************************************************/
/* Segment Sub-Allocation block structure declares.                    */
/* Next block is an implementation dependent segment suballoc save     */
/* structure used to keep the suballocation system offsets and to      */
/* restore them if needed. This scheme implies that no SubFree be used */
/***********************************************************************/
typedef struct {               /* Plug Area SubAlloc header            */
  size_t To_Free;              /* Offset of next free block            */
  size_t FreeBlk;              /* Size of remaining free memory        */
  } POOLHEADER, *PPOOLHEADER;

/***********************************************************************/
/*  Language block. Containing all global information for the language */
/*  this block is saved and retrieved with the language. Information   */
/*  in this block can be set and modified under Grammar editing.       */
/***********************************************************************/
#if defined(BIT64)
typedef int    TIME_T;              /* Lang block size must not change */
#else    // BIT32
typedef time_t TIME_T;              /* time_t                          */
#endif   // BIT32

typedef struct {
  uint Memsize;
  uint Size;
  } AREADEF;

typedef struct Lang_block {
  NAME     LangName;                /* Language name                   */
  NAME     Application;             /* Application name                */
  } LANG, *PLANG;

/***********************************************************************/
/*  Application block. It contains all global information for the      */
/*  current parse and execution using the corresponding language.      */
/*  This block is dynamically allocated and set at language init.      */
/***********************************************************************/
typedef struct _activity {          /* Describes activity and language */
  void     *Aptr;                   /* Points to user work area(s)     */
  NAME      Ap_Name;                /* Current application name        */
  } ACTIVITY;

/*----------------  UNIT ??????????    VERSION ? ----------------------*/
typedef struct _parm {
  union {
    void *Value;
    int   Intval;
    }; // end union
  short Type, Domain;
  PPARM Next;
  } PARM;

/***********************************************************************/
/*  Global Structure Block.  This block contains, or points to, all    */
/*  information used by CONNECT tables.  Passed as an argument         */
/*  to any routine allows it to have access to the entire information  */
/*  currently available for the whole set of loaded languages.         */
/***********************************************************************/
typedef struct _global {            /* Global structure                */
  void     *Sarea;                  /* Points to work area             */
  size_t    Sarea_Size;             /* Work area size                  */
	PACTIVITY Activityp;
  char      Message[MAX_STR];				/* Message (result, error, trace)  */
	size_t    More;										/* Used by jsonudf                 */
	size_t    Saved_Size;             /* Saved work area to_free         */
	bool      Createas;               /* To pass multi to ext tables     */
  void     *Xchk;                   /* indexes in create/alter         */
  short     Alchecked;              /* Checked for ALTER               */
  short     Mrr;                    /* True when doing mrr             */
  int       N;                      /* Utility                         */
  int       jump_level;
  jmp_buf   jumper[MAX_JUMP + 2];
  } GLOBAL;

/***********************************************************************/
/*  Exported routine declarations.                                     */
/***********************************************************************/
#if defined(XMSG)
DllExport char   *PlugReadMessage(PGLOBAL, int, char *);
#elif defined(NEWMSG)
DllExport char   *PlugGetMessage(PGLOBAL, int);
#endif   // XMSG  || NEWMSG
#if defined(_WIN32)
DllExport short   GetLineLength(PGLOBAL);   // Console line length
#endif   // _WIN32
DllExport PGLOBAL PlugInit(LPCSTR, size_t); // Plug global initialization
DllExport PGLOBAL PlugExit(PGLOBAL);        // Plug global termination
DllExport LPSTR   PlugRemoveType(LPSTR, LPCSTR);
DllExport LPCSTR  PlugSetPath(LPSTR to, LPCSTR prefix, LPCSTR name, LPCSTR dir);
DllExport BOOL    PlugIsAbsolutePath(LPCSTR path);
DllExport bool    AllocSarea(PGLOBAL, size_t);
DllExport void    FreeSarea(PGLOBAL);
DllExport BOOL    PlugSubSet(void *, size_t);
DllExport void   *PlugSubAlloc(PGLOBAL, void *, size_t);
DllExport char   *PlugDup(PGLOBAL g, const char *str);
DllExport void    htrc(char const *fmt, ...);
DllExport void    xtrc(uint, char const* fmt, ...);
DllExport uint    GetTraceValue(void);
DllExport void*   MakePtr(void* memp, size_t offset);
DllExport size_t  MakeOff(void* memp, void* ptr);

#if defined(__cplusplus)
} // extern "C"
#endif

/*-------------------------- End of Global.H --------------------------*/
