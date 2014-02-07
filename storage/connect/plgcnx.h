/**************************************************************************/
/*  PLGCNX.H                                                              */
/*  Copyright to the author: Olivier Bertrand         2000-2014           */
/*                                                                        */
/*  This is the connection DLL's declares.                                */
/**************************************************************************/
#if !defined(_PLGCNX_H)
#define _PLGCNX_H

#define MAXMSGLEN    65512           /* Default max length of cnx message */
#define MAXERRMSG      512           /* Max length of error messages      */
#define MAXMESSAGE     256           /* Max length of returned messages   */
#define MAXDBNAME      128           /* Max length of DB related names    */

/**************************************************************************/
/*  API Function return codes.                                            */
/**************************************************************************/
enum FNRC {RC_LICENSE   =   7,       /* PLGConnect prompt for license key */
           RC_PASSWD    =   6,       /* PLGConnect prompt for User/Pwd    */
           RC_SUCWINFO  =   5,       /* Succes With Info return code      */
           RC_SOCKET    =   4,       /* RC from PLGConnect to socket DLL  */
           RC_PROMPT    =   3,       /* Intermediate prompt return        */
           RC_CANCEL    =   2,       /* Command was cancelled by user     */
           RC_PROGRESS  =   1,       /* Intermediate progress info        */
           RC_SUCCESS   =   0,       /* Successful function (must be 0)   */
           RC_MEMORY    =  -1,       /* Storage allocation error          */
           RC_TRUNCATED =  -2,       /* Result has been truncated         */
           RC_TIMEOUT   =  -3,       /* Connection timeout occured        */
           RC_TOOBIG    =  -4,       /* Data is too big for connection    */
           RC_KEY       =  -5,       /* Null ptr to key in Connect        */
                                     /*   or bad key in other functions   */
           RC_MAXCONN   =  -6,       /* Too many conn's for one process   */
           RC_MAXCLIENT =  -7,       /* Too many clients for one system   */
           RC_SYNCHRO   =  -8,       /* Synchronization error             */
           RC_SERVER    =  -9,       /* Error related to the server       */
           RC_MAXCOL    = -10,       /* Result has too many columns       */
           RC_LAST      = -10};      /* Other error codes are < this and  */
                                     /*   are system errors.              */

/**************************************************************************/
/*  Standard function return codes.                                       */
/**************************************************************************/
#if !defined(RC_OK_DEFINED)
#define RC_OK_DEFINED
enum RCODE {RC_OK      =   0,        /* No error return code              */
            RC_NF      =   1,        /* Not found return code             */
            RC_EF      =   2,        /* End of file return code           */
            RC_FX      =   3,        /* Error return code                 */
            RC_INFO    =   4};       /* Success with info                 */
#endif   // !RC_OK_DEFINED

/**************************************************************************/
/*  Index of info values within the info int integer array.              */
/**************************************************************************/
enum INFO {INDX_RC,                  /* Index of PlugDB return code field */
           INDX_TIME,                /* Index of elapsed time in millisec */
           INDX_CHG,                 /* Index of Language or DB changed   */
           INDX_RSAV,                /* Index of Result Set availability  */
           INDX_TYPE,                /* Index of returned data type field */
           INDX_LINE,                /* Index of number of lines field    */
           INDX_LEN,                 /* Index of line length field        */
           INDX_SIZE,                /* Index of returned data size field */
           INDX_MAX};                /* Size of info array                */

#ifdef NOT_USED
/**************************************************************************/
/*  Internal message types.                                               */
/**************************************************************************/
enum MSGTYP {MST_OPEN      = 10,     /* Code for old connecting message   */
             MST_COMMAND   = 11,     /* Code for send command message     */
             MST_RESULT    = 12,     /* Code for get result message       */
             MST_CLOSE     = 13,     /* Code for disconnecting message    */
             MST_PROGRESS  = 14,     /* Code for progress message         */
             MST_CANCEL    = 15,     /* Code for cancel message           */
             MST_PROCESSED = 16,     /* Code for already processed msg    */
             MST_ERROR     = 17,     /* Code for get error message        */
             MST_CHAR      = 18,     /* Code for get char value message   */
             MST_LONG      = 19,     /* Code for get int value message   */
             MST_COLUMN    = 20,     /* Code for get col  value message   */
             MST_MESSAGE   = 21,     /* Code for get message    message   */
             MST_HEADER    = 22,     /* Code for get header     message   */
             MST_SOCKET    = 23,     /* Code for socket error   message   */
             MST_SHUTDOWN  = 24,     /* Code for shutdown       message   */
             MST_SOCKPROG  = 25,     /* Code for socket progress message  */
             MST_POST      = 26,     /* Code for post command message     */
             MST_NEW_OPEN  = 27,     /* Code for new connecting message   */
             MST_PROG_NUM  =  5};    /* Num of integers in progress msg   */

/**************************************************************************/
/*  Vendors.                                                              */
/**************************************************************************/
enum VENDOR {VDR_UNKNOWN   = -2,     /* Not known or not connected        */
             VDR_PlugDB    = -1,     /* PlugDB                           */
             VDR_OTHER     =  0};    /* OEM                               */

/**************************************************************************/
/*  Attribute keys of Result Description structure (arranged by type).    */
/**************************************************************************/
enum CKEYS {K_ProgMsg, K_Lang, K_ActiveDB, K_Cmax};
enum LKEYS {K_NBcol, K_NBlin, K_CurPos, K_RC, K_Result, K_Elapsed,
            K_Continued, K_Maxsize, K_Affrows, K_Lmax, K_Maxcol,
            K_Maxres, K_Maxlin, K_NBparm};
enum NKEYS {K_Type, K_Length, K_Prec, K_DataLen, K_Unsigned, K_Nmax};

/**************************************************************************/
/*  Result description structures.                                        */
/**************************************************************************/
typedef struct _MsgTagAttr {
  bool   fSupplied;
  char   Attr[MAXMESSAGE];
  } MTAG, *PMTAG;

typedef struct _CharTagAttr {
  bool   fSupplied;
  char   Attr[MAXDBNAME];
  } CTAG, *PCTAG;

typedef struct _LongTagAttr {
  bool   fSupplied;
  int   Attr;
  } LTAG, *PLTAG;

typedef struct _ColVar {
  LTAG   Lat[K_Nmax];
  CTAG   Cat;
  } COLVAR, *LPCOLVAR;

typedef struct _ResDesc {
  int   Maxcol;                                 /* Max number of columns */
  int   Colnum;                                 /* Number of columns     */
  MTAG   Mat;                                    /* Message               */
  CTAG   Cat[K_Cmax];                            /* Character attributes  */
  LTAG   Lat[K_Lmax];                            /* Long int  attributes  */
  COLVAR Col[1];                                 /* Column    attributes  */
  } RDESC, *PRDESC;

/**************************************************************************/
/*  Exported PlugDB client functions in Plgcnx DLL.                       */
/**************************************************************************/
#if !defined(CNXFUNC)
#if defined(UNIX) || defined(UNIV_LINUX)
#undef __stdcall
#define __stdcall
#endif

#if defined(NOLIB)                         /* Dynamic link of plgcnx.dll  */
#define CNXFUNC(f)    (__stdcall *f)
#else      /* LIB */                       /* Static link with plgcnx.lib */
#define CNXFUNC(f)    __stdcall f
#endif
#endif

#if !defined(CNXKEY)
#define CNXKEY         uint
#endif

#if !defined(XTRN)
#define XTRN
#endif

//#if !defined(NO_FUNC)
#ifdef __cplusplus
extern "C" {
#endif

XTRN int CNXFUNC(PLGConnect)     (CNXKEY *, const char *, bool);
XTRN int CNXFUNC(PLGSendCommand) (CNXKEY, const char *, void *, int, int *);
XTRN int CNXFUNC(PLGGetResult)   (CNXKEY, void *, int, int *, bool);
XTRN int CNXFUNC(PLGDisconnect)  (CNXKEY);
XTRN int CNXFUNC(PLGGetErrorMsg) (CNXKEY, char *, int, int *);
XTRN bool CNXFUNC(PLGGetCharValue)(CNXKEY, char *, int, int);
XTRN bool CNXFUNC(PLGGetIntValue)(CNXKEY, int *, int);
XTRN bool CNXFUNC(PLGGetColValue) (CNXKEY, int *, int, int);
XTRN bool CNXFUNC(PLGGetMessage)  (CNXKEY, char *, int);
XTRN bool CNXFUNC(PLGGetHeader)   (CNXKEY, char *, int, int, int);

#ifdef __cplusplus
}
#endif
//#endif /* !NO_FUNC */

/**************************************************************************/
/*  Convenience function Definitions                                      */
/**************************************************************************/
#define PLGPostCommand(T,C)    PLGSendCommand(T,C,NULL,0,NULL)
#if defined(FNCMAC)
#define PLGGetProgMsg(T,C,S)   PLGGetCharValue(T,C,S,K_ProgMsg)
#define PLGGetLangID(T,C,S)    PLGGetCharValue(T,C,S,K_Lang)
#define PLGGetActiveDB(T,C,S)  PLGGetCharValue(T,C,S,K_ActiveDB)
#define PLGGetCursorPos(T,L)   PLGGetIntValue(T,L,K_CurPos)
#define PLGGetResultType(T,L)  PLGGetIntValue(T,L,K_Result)
#define PLGGetNBcol(T,L)       PLGGetIntValue(T,L,K_NBcol)
#define PLGGetNBlin(T,L)       PLGGetIntValue(T,L,K_NBlin)
#define PLGGetRetCode(T,L)     PLGGetIntValue(T,L,K_RC)
#define PLGGetElapsed(T,L)     PLGGetIntValue(T,L,K_Elapsed)
#define PLGGetContinued(T,L)   PLGGetIntValue(T,L,K_Continued)
#define PLGGetMaxSize(T,L)     PLGGetIntValue(T,L,K_Maxsize)
#define PLGGetLength(T,L,C)    PLGGetColValue(T,L,K_Length,C)
#define PLGGetDataSize(T,L,C)  PLGGetColValue(T,L,K_DataLen,C)
#define PLGGetDecimal(T,L,C)   PLGGetColValue(T,L,K_Prec,C)
#define PLGGetType(T,L,C)      PLGGetColValue(T,L,K_Type,C)
#endif  /* FNCMAC */
#endif // NOT_USED

#endif  /* !_PLGCNX_H */

/* ------------------------- End of Plgcnx.h ---------------------------- */
