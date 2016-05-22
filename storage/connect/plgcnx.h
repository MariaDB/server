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
           RC_TIMEOUT   =  -3,       /* Connection timeout occurred        */
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

#endif  /* !_PLGCNX_H */

/* ------------------------- End of Plgcnx.h ---------------------------- */
