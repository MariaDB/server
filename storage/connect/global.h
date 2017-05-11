/***********************************************************************/
/*  GLOBAL.H: Declaration file used by all CONNECT implementations.    */
/*  (C) Copyright Olivier Bertrand                       1993-2017     */
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

#if defined(__WIN__) && !defined(NOEX)
#define DllExport  __declspec( dllexport )
#else   // !__WIN__
#define DllExport
#endif  // !__WIN__

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

#if defined(__WIN__)
#define CRLF  2
#else    // !__WIN__
#define CRLF  1
#endif  // !__WIN__

/***********************************************************************/
/*  Define access to the thread based trace value.                     */
/***********************************************************************/
#define trace  GetTraceValue()

/***********************************************************************/
/*  Miscellaneous Constants                                            */
/***********************************************************************/
#define  NO_IVAL   -95684275        /* Used by GetIntegerOption        */
#define  VMLANG          370        /* Size of olf VM lang blocks      */
#define  MAX_JUMP         24        /* Maximum jump level number       */
#define  MAX_STR        4160        /* Maximum message length          */
#define  STR_SIZE        501        /* Length of char strings.         */
#define  STD_INPUT         0        /* Standard language input         */
#define  STD_OUTPUT        1        /* Standard language output        */
#define  ERROR_OUTPUT      2        /* Error    message  output        */
#define  DEBUG_OUTPUT      3        /* Debug    info     output        */
#define  PROMPT_OUTPUT     4        /* Prompt   message  output        */
#define  COPY_OUTPUT       5        /* Copy of  language input         */
#define  STD_MSG           6        /* System message file             */
#define  DEBUG_MSG         7        /* Debug  message file             */
#define  DUMMY             0        /* Dummy  file index in Ldm block  */
#define  STDIN             1        /* stdin  file index in Ldm block  */
#define  STDOUT            2        /* stdout file index in Ldm block  */
#define  STDERR            3        /* stderr file index in Ldm block  */
#define  STDEBUG           4        /* debug  file index in Ldm block  */
#define  STDPRN            5        /* stdprn file index in Ldm block  */
#define  STDFREE           6        /* Free   file index in Ldm block  */

#define  TYPE_SEM         -2        /* Returned semantic function      */
#define  TYPE_DFONC       -2        /* Indirect sem ref in FPARM       */
#define  TYPE_VOID        -1
#define  TYPE_SBPAR       -1        /* Phrase reference in FPARM       */
#define  TYPE_SEMX         0        /* Initial semantic function type? */
#define  TYPE_ERROR        0
#define  TYPE_STRING       1
#define  TYPE_DOUBLE       2
#define  TYPE_SHORT        3
#define  TYPE_TINY         4
#define  TYPE_BIGINT       5
#define  TYPE_LIST         6
#define  TYPE_INT          7
#define  TYPE_DECIM        9
#define  TYPE_BIN         10
#define  TYPE_PCHAR       11

#if defined(OS32)
  #define  SYS_STAMP   "OS32"
#elif defined(UNIX) || defined(LINUX) || defined(UNIV_LINUX)
  #define  SYS_STAMP   "UNIX"
#elif defined(OS16)
  #define  SYS_STAMP   "OS16"
#elif defined(DOSR)
  #define  SYS_STAMP   "DOSR"
#elif defined(WIN)
  #define  SYS_STAMP   "WIN1"
#elif defined(__WIN__)
  #define  SYS_STAMP   "WIN2"
#else
  #define  SYS_STAMP   "XXXX"
#endif

#if defined(__cplusplus)
extern "C" {
#endif

/***********************************************************************/
/*  Static variables                                                   */
/***********************************************************************/
#if defined(STORAGE)
         char      sys_stamp[5] = SYS_STAMP;
#else
  extern char      sys_stamp[];
#endif

/***********************************************************************/
/*                       File-Selection Indicators                     */
/***********************************************************************/
#define PAT_LOG       "log"

#if defined(UNIX) || defined(LINUX) || defined(UNIV_LINUX)
  /*********************************************************************/
  /*  printf does not accept null pointer for %s target.               */
  /*********************************************************************/
  #define SVP(S)  ((S) ? S : "<null>")
#else
  /*********************************************************************/
  /*  printf accepts null pointer for %s target.                       */
  /*********************************************************************/
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

typedef uint  OFFSET;
typedef char  NAME[9];

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

/***********************************************************************/
/* Segment Sub-Allocation block structure declares.                    */
/* Next block is an implementation dependent segment suballoc save     */
/* structure used to keep the suballocation system offsets and to      */
/* restore them if needed. This scheme implies that no SubFree be used */
/***********************************************************************/
typedef struct {               /* Plug Area SubAlloc header            */
  OFFSET To_Free;              /* Offset of next free block            */
  uint   FreeBlk;              /* Size of remaining free memory        */
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
  uint      Sarea_Size;             /* Work area size                  */
	PACTIVITY Activityp;
  char      Message[MAX_STR];
	ulong     More;										/* Used by jsonudf                 */
	int       Createas;               /* To pass info to created table   */
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
#if defined(__WIN__)
DllExport short   GetLineLength(PGLOBAL);  // Console line length
#endif   // __WIN__
DllExport PGLOBAL PlugInit(LPCSTR, uint);  // Plug global initialization
DllExport int     PlugExit(PGLOBAL);       // Plug global termination
DllExport LPSTR   PlugRemoveType(LPSTR, LPCSTR);
DllExport LPCSTR  PlugSetPath(LPSTR to, LPCSTR prefix, LPCSTR name, LPCSTR dir);
DllExport BOOL    PlugIsAbsolutePath(LPCSTR path);
DllExport void   *PlugAllocMem(PGLOBAL, uint);
DllExport BOOL    PlugSubSet(PGLOBAL, void *, uint);
DllExport void   *PlugSubAlloc(PGLOBAL, void *, size_t);
DllExport char   *PlugDup(PGLOBAL g, const char *str);
DllExport void   *MakePtr(void *, OFFSET);
DllExport void    htrc(char const *fmt, ...);
DllExport int     GetTraceValue(void);

#if defined(__cplusplus)
} // extern "C"
#endif

/*-------------------------- End of Global.H --------------------------*/
