/************** PlgDBSem H Declares Source Code File (.H) **************/
/*  Name: CHKLVL.H  Version 1.1                                        */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2009         */
/*                                                                     */
/*  This file contains the definition of the checking level constants. */
/***********************************************************************/

#if !defined(_CHKLVL_DEFINED_)
#define      _CHKLVL_DEFINED_
/***********************************************************************/
/*  Following definitions are used to indicate the level of checking.  */
/***********************************************************************/
enum CHKLVL {CHK_NO      = 0x00,      /* No checking                   */
             CHK_TYPE    = 0x01,      /* Check types for Insert/Update */
             CHK_UPDATE  = 0x02,      /* Two pass checking of Update   */
             CHK_DELETE  = 0x04,      /* Indexed checking of Delete    */
             CHK_JOIN    = 0x08,      /* Check types joining tables    */
             CHK_OPT     = 0x10,      /* Automatic optimize on changes */
             CHK_MANY    = 0x20,      /* Check many-to-many joins      */
             CHK_ALL     = 0x3F,      /* All of the above              */
             CHK_STD     = 0x1E,      /* Standard level of checking    */
             CHK_MAXRES  = 0x40,      /* Prevent Maxres recalculation  */
             CHK_ONLY    = 0x100};    /* Just check, no action (NIY)   */

/***********************************************************************/
/*  Following definitions are used to indicate the execution mode.     */
/***********************************************************************/
enum XMOD {XMOD_EXECUTE =  0,         /* DOS execution mode            */
           XMOD_PREPARE =  1,         /* Prepare mode                  */
           XMOD_TEST    =  2,         /* Test mode                     */
           XMOD_CONVERT =  3};        /* HQL conversion mode           */

/***********************************************************************/
/*  Following definitions indicate the use of a temporay file.         */
/***********************************************************************/
enum USETEMP {TMP_NO    =  0,         /* Never                         */
              TMP_AUTO  =  1,         /* Best choice                   */
              TMP_YES   =  2,         /* Always                        */
              TMP_FORCE =  3,         /* Forced for MAP tables         */
              TMP_TEST  =  4};        /* Testing value                 */

/***********************************************************************/
/*  Following definitions indicate conversion of TEXT columns.         */
/***********************************************************************/
enum TYPCONV {TPC_NO   =  0,          /* Never                         */
              TPC_YES  =  1,          /* Always                        */
							TPC_FORCE = 2,          /* Also convert BLOBs            */
							TPC_SKIP =  3};         /* Skip TEXT columns             */

#endif    // _CHKLVL_DEFINED_
