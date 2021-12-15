/************* TabTbl C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: TABTBL                                                */
/* -------------                                                       */
/*  Version 1.9                                                        */
/*                                                                     */
/*  Author: Olivier BERTRAND                              2008-2018    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the TDBTBL class DB routines.                     */
/*                                                                     */
/* WHAT YOU NEED TO COMPILE THIS PROGRAM:                              */
/* --------------------------------------                              */
/*                                                                     */
/*  REQUIRED FILES:                                                    */
/*  ---------------                                                    */
/*    TABTBL.CPP     - Source code                                     */
/*    PLGDBSEM.H     - DB application declaration file                 */
/*    TABDOS.H       - TABDOS classes declaration file                 */
/*    TABTBL.H       - TABTBL classes declaration file                 */
/*    GLOBAL.H       - Global declaration file                         */
/*                                                                     */
/*  REQUIRED LIBRARIES:                                                */
/*  -------------------                                                */
/*    Large model C library                                            */
/*                                                                     */
/*  REQUIRED PROGRAMS:                                                 */
/*  ------------------                                                 */
/*    IBM, Borland, GNU or Microsoft C++ Compiler and Linker           */
/*                                                                     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant section of system dependant header files.         */
/***********************************************************************/
//#include "sql_base.h"
#include "my_global.h"
#include "table.h"       // MySQL table definitions
#if defined(_WIN32)
#include <stdlib.h>
#include <stdio.h>
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif
//#include <windows.h>
#else
#if defined(UNIX)
#include <fnmatch.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "osutil.h"
#else
//#include <io.h>
#endif
//#include <fcntl.h>
#endif

/***********************************************************************/
/*  Include application header files:                                  */
/***********************************************************************/
#include "global.h"      // global declarations
#include "plgdbsem.h"    // DB application declarations
#include "reldef.h"      // DB definition declares
#include "filamtxt.h"
#include "tabcol.h"
#include "tabdos.h"      // TDBDOS and DOSCOL class dcls
#include "tabtbl.h"
#include "tabext.h"
#include "tabmysql.h"
#include "ha_connect.h"

#if defined(_WIN32)
#if defined(__BORLANDC__)
#define SYSEXIT void _USERENTRY
#else
#define SYSEXIT void
#endif
#else   // !_WIN32
#define SYSEXIT void *
#endif  // !_WIN32

extern pthread_mutex_t tblmut;

/* ---------------------------- Class TBLDEF ---------------------------- */

/**************************************************************************/
/*  Constructor.                                                          */
/**************************************************************************/
TBLDEF::TBLDEF(void)
  {
//To_Tables = NULL;
  Accept = false;
  Thread = false;
  Maxerr = 0;
  Ntables = 0;
  Pseudo = 3;
  } // end of TBLDEF constructor

/**************************************************************************/
/*  DefineAM: define specific AM block values from XDB file.              */
/**************************************************************************/
bool TBLDEF::DefineAM(PGLOBAL g, LPCSTR, int)
  {
  char   *tablist, *dbname, *def = NULL;

  Desc = "Table list table";
  tablist = GetStringCatInfo(g, "Tablist", "");
  dbname = GetStringCatInfo(g, "Dbname", "*");
  def = GetStringCatInfo(g, "Srcdef", NULL);
  Ntables = 0;

  if (*tablist) {
    char  *p, *pn, *pdb;
    PTABLE tbl;

    for (pdb = tablist; ;) {
      if ((p = strchr(pdb, ',')))
        *p = 0;

      // Analyze the table name, it may have the format:
      // [dbname.]tabname
      if ((pn = strchr(pdb, '.'))) {
        *pn++ = 0;
      } else {
        pn = pdb;
        pdb = dbname;
      } // endif p

      // Allocate the TBLIST block for that table
      tbl = new(g) XTAB(pn, def);
      tbl->SetSchema(pdb);
      
      if (trace(1))
        htrc("TBL: Name=%s db=%s\n", tbl->GetName(), tbl->GetSchema());

      // Link the blocks
      if (Tablep)
        Tablep->Link(tbl);
      else
        Tablep = tbl;

      Ntables++;

      if (p)
        pdb = pn + strlen(pn) + 1;
      else
        break;

      } // endfor pdb

    Maxerr = GetIntCatInfo("Maxerr", 0);
    Accept = GetBoolCatInfo("Accept", false);
    Thread = GetBoolCatInfo("Thread", false);
    } // endif tablist

  return FALSE;
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB TBLDEF::GetTable(PGLOBAL g, MODE)
  {
  if (Catfunc == FNC_COL)
    return new(g) TDBTBC(this);
	else if (Thread) {
#if defined(DEVELOPMENT)
		return new(g) TDBTBM(this);
#else
		strcpy(g->Message, "Option THREAD is no more supported");
		return NULL;
#endif   // DEVELOPMENT
	} else
    return new(g) TDBTBL(this);

  } // end of GetTable

/* ------------------------- Class TDBTBL ---------------------------- */

/***********************************************************************/
/*  TDBTBL constructors.                                               */
/***********************************************************************/
TDBTBL::TDBTBL(PTBLDEF tdp) : TDBPRX(tdp)
  {
  Tablist = NULL;
  CurTable = NULL;
//Tdbp = NULL;
  Accept = tdp->Accept;
  Maxerr = tdp->Maxerr;
  Nbc = 0;
  Rows = 0;
  Crp = 0;
//  NTables = 0;
//  iTable = 0;
  } // end of TDBTBL standard constructor

/***********************************************************************/
/*  Allocate TBL column description block.                             */
/***********************************************************************/
PCOL TDBTBL::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  return new(g) PRXCOL(cdp, this, cprec, n);
  } // end of MakeCol

/***********************************************************************/
/*  InsertSpecialColumn: Put a special column ahead of the column list.*/
/***********************************************************************/
PCOL TDBTBL::InsertSpecialColumn(PCOL scp)
  {
  PCOL colp;

  if (!scp->IsSpecial())
    return NULL;

  if (scp->GetAmType() == TYPE_AM_TABID)
    // This special column is handled locally
    colp = new((TIDBLK*)scp) TBTBLK(scp->GetValue());
  else  // Other special columns are treated normally
    colp = scp;

  colp->SetNext(Columns);
  Columns = colp;
  return colp;
  } // end of InsertSpecialColumn

/***********************************************************************/
/*  Initializes the table table list.                                  */
/***********************************************************************/
bool TDBTBL::InitTableList(PGLOBAL g)
  {
  int     n;
  uint    sln;
  const char   *scs;
  PTABLE  tp, tabp;
  PCOL    colp;
  PTBLDEF tdp = (PTBLDEF)To_Def;
  PCATLG  cat = To_Def->GetCat();
  PHC     hc = ((MYCAT*)cat)->GetHandler();

  scs = hc->get_table()->s->connect_string.str;
  sln = hc->get_table()->s->connect_string.length;
//  PlugSetPath(filename, Tdbp->GetFile(g), Tdbp->GetPath());

  for (n = 0, tp = tdp->Tablep; tp; tp = tp->GetNext()) {
    if (TestFil(g, To_CondFil, tp)) {
      tabp = new(g) XTAB(tp);

      if (tabp->GetSrc()) {
        // Table list is a list of connections
        hc->get_table()->s->connect_string.str = (char*)tabp->GetName();
        hc->get_table()->s->connect_string.length = strlen(tabp->GetName());
        } // endif Src

      // Get the table description block of this table
      if (!(Tdbp = GetSubTable(g, tabp))) {
        if (++Nbc > Maxerr)
          return TRUE;               // Error return
        else
          continue;                  // Skip this table

      } else
        RemoveNext(tabp);            // To avoid looping

      // We must allocate subtable columns before GetMaxSize is called
      // because some (PLG, ODBC?) need to have their columns attached.
      // Real initialization will be done later.
      for (colp = Columns; colp; colp = colp->GetNext())
        if (!colp->IsSpecial())
          if (((PPRXCOL)colp)->Init(g, NULL) && !Accept)
            return TRUE;

      if (Tablist)
        Tablist->Link(tabp);
      else
        Tablist = tabp;

      n++;
      } // endif filp

    } // endfor tp

  hc->get_table()->s->connect_string.str = (char*)scs;
  hc->get_table()->s->connect_string.length = sln;

//NumTables = n;
  To_CondFil = NULL;        // To avoid doing it several times
  return FALSE;
  } // end of InitTableList

/***********************************************************************/
/*  Test the tablename against the pseudo "local" filter.              */
/***********************************************************************/
bool TDBTBL::TestFil(PGLOBAL g, PCFIL filp, PTABLE tabp)
  {
  char *body, *fil, op[8], tn[NAME_LEN];
  bool  neg;

  if (!filp)
    return TRUE;
  else
    body = filp->Body;

  if (strstr(body, " OR ") || strstr(body, " AND "))
    return TRUE;               // Not handled yet
  else
    fil = body + (*body == '(' ? 1 : 0);

  if (sscanf(fil, "TABID %s", op) != 1)
    return TRUE;               // ignore invalid filter

  if ((neg = !strcmp(op, "NOT")))
    strcpy(op, "IN");

  if (!strcmp(op, "=")) {
    // Temporarily, filter must be "TABID = 'value'" only
    if (sscanf(fil, "TABID = '%[^']'", tn) != 1)
      return TRUE;             // ignore invalid filter

    return !stricmp(tn, tabp->GetName());
  } else if (!strcmp(op, "IN")) {
    char *p, *tnl = (char*)PlugSubAlloc(g, NULL, strlen(fil) - 10);
    int   n;

    if (neg)
      n = sscanf(fil, "TABID NOT IN (%[^)])", tnl);
    else
      n = sscanf(fil, "TABID IN (%[^)])", tnl);

    if (n != 1)
      return TRUE;             // ignore invalid filter

    while (tnl) {
      if ((p = strchr(tnl, ',')))
        *p++ = 0;

      if (sscanf(tnl, "'%[^']'", tn) != 1)
        return TRUE;           // ignore invalid filter
      else if (!stricmp(tn, tabp->GetName()))
        return !neg;           // Found

      tnl = p;
      } // endwhile

    return neg;                // Not found
  } // endif op

  return TRUE;                 // invalid operator
  } // end of TestFil

/***********************************************************************/
/*  Sum up the cardinality of all sub-tables.                          */
/***********************************************************************/
int TDBTBL::Cardinality(PGLOBAL g)
  {
  if (!g)
    return 0;                 // Cannot make the table list
  else if (Cardinal < 0) {
    int tsz;

    if (!Tablist && InitTableList(g))
      return 0;               // Cannot be calculated at this stage

    Cardinal = 0;

    for (PTABLE tabp = Tablist; tabp; tabp = tabp->GetNext()) {
      if ((tsz = tabp->GetTo_Tdb()->Cardinality(g)) < 0) {
        Cardinal = -1;
        return tsz;
        } // endif mxsz

      Cardinal += tsz;
      } // endfor i

    } // endif Cardinal

  return Cardinal;
  } // end of Cardinality

/***********************************************************************/
/*  Sum up the maximum sizes of all sub-tables.                        */
/***********************************************************************/
int TDBTBL::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0) {
    int mxsz;

    if (!Tablist && InitTableList(g))
      return 0;               // Cannot be calculated at this stage

    MaxSize = 0;

    for (PTABLE tabp = Tablist; tabp; tabp = tabp->GetNext()) {
      if ((mxsz = tabp->GetTo_Tdb()->GetMaxSize(g)) < 0) {
        MaxSize = -1;
        return mxsz;
        } // endif mxsz

      MaxSize += mxsz;
      } // endfor i

    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  Reset read/write position values.                                  */
/***********************************************************************/
void TDBTBL::ResetDB(void)
  {
  for (PCOL colp = Columns; colp; colp = colp->GetNext())
    if (colp->GetAmType() == TYPE_AM_TABID ||
        colp->GetAmType() == TYPE_AM_SRVID)
      colp->COLBLK::Reset();

  for (PTABLE tabp = Tablist; tabp; tabp = tabp->GetNext())
    tabp->GetTo_Tdb()->ResetDB();

  Tdbp = Tablist->GetTo_Tdb();
  Crp = 0;
  } // end of ResetDB

/***********************************************************************/
/*  Returns RowId if b is false or Rownum if b is true.                */
/***********************************************************************/
int TDBTBL::RowNumber(PGLOBAL g, bool b)
  {
  return Tdbp->RowNumber(g) + ((b) ? 0 : Rows);
  } // end of RowNumber

/***********************************************************************/
/*  TBL Access Method opening routine.                                 */
/*  Open first file, other will be opened sequencially when reading.   */
/***********************************************************************/
bool TDBTBL::OpenDB(PGLOBAL g)
  {
  if (trace(1))
    htrc("TBL OpenDB: tdbp=%p tdb=R%d use=%d key=%p mode=%d\n",
                      this, Tdb_No, Use, To_Key_Col, Mode);

  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, replace it at its beginning.               */
    /*******************************************************************/
    ResetDB();
    return Tdbp->OpenDB(g);  // Re-open fist table
    } // endif use

  /*********************************************************************/
  /*  When GetMaxsize was called, To_CondFil was not set yet.          */
  /*********************************************************************/
  if (To_CondFil && Tablist) {
    Tablist = NULL;
    Nbc = 0;
    } // endif To_CondFil

  /*********************************************************************/
  /*  Open the first table of the list.                                */
  /*********************************************************************/
  if (!Tablist && InitTableList(g))     //  done in GetMaxSize
    return TRUE;

  if ((CurTable = Tablist)) {
    Tdbp = CurTable->GetTo_Tdb();
//  Tdbp->SetMode(Mode);
//  Tdbp->ResetDB();
//  Tdbp->ResetSize();

    // Check and initialize the subtable columns
    for (PCOL cp = Columns; cp; cp = cp->GetNext())
      if (cp->GetAmType() == TYPE_AM_TABID)
        cp->COLBLK::Reset();
      else if (((PPRXCOL)cp)->Init(g, NULL) && !Accept)
        return TRUE;
        
    if (trace(1))
      htrc("Opening subtable %s\n", Tdbp->GetName());

    // Now we can safely open the table
    if (Tdbp->OpenDB(g))
      return TRUE;

    } // endif *Tablist

  Use = USE_OPEN;
  return FALSE;
  } // end of OpenDB

/***********************************************************************/
/*  ReadDB: Data Base read routine for MUL access method.              */
/***********************************************************************/
int TDBTBL::ReadDB(PGLOBAL g)
  {
  int rc;

  if (!CurTable)
    return RC_EF;
  else if (To_Kindex) {
    /*******************************************************************/
    /*  Reading is by an index table.                                  */
    /*******************************************************************/
    strcpy(g->Message, MSG(NO_INDEX_READ));
    rc = RC_FX;
  } else {
    /*******************************************************************/
    /*  Now start the reading process.                                 */
    /*******************************************************************/
   retry:
    rc = Tdbp->ReadDB(g);

    if (rc == RC_EF) {
      // Total number of rows met so far
      Rows += Tdbp->RowNumber(g) - 1;
      Crp += Tdbp->GetProgMax(g);

      if ((CurTable = CurTable->GetNext())) {
        /***************************************************************/
        /*  Continue reading from next table file.                     */
        /***************************************************************/
        Tdbp->CloseDB(g);
        Tdbp = CurTable->GetTo_Tdb();

        // Check and initialize the subtable columns
        for (PCOL cp = Columns; cp; cp = cp->GetNext())
          if (cp->GetAmType() == TYPE_AM_TABID ||
              cp->GetAmType() == TYPE_AM_SRVID)
            cp->COLBLK::Reset();
          else if (((PPRXCOL)cp)->Init(g, NULL) && !Accept)
            return RC_FX;

        if (trace(1))
          htrc("Opening subtable %s\n", Tdbp->GetName());

        // Now we can safely open the table
        if (Tdbp->OpenDB(g))     // Open next table
          return RC_FX;

        goto retry;
        } // endif iFile

    } else if (rc == RC_FX)
      strcat(strcat(strcat(g->Message, " ("), Tdbp->GetName()), ")");

  } // endif To_Kindex

  return rc;
  } // end of ReadDB

/* ---------------------------- TBTBLK ------------------------------- */

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void TBTBLK::ReadColumn(PGLOBAL)
  {
  if (trace(1))
    htrc("TBT ReadColumn: name=%s\n", Name);

  Value->SetValue_psz((char*)((PTDBTBL)To_Tdb)->Tdbp->GetName());

  } // end of ReadColumn

#if defined(DEVELOPMENT)
/* ------------------------- Class TDBTBM ---------------------------- */

/***********************************************************************/
/*  Thread routine that check and open one remote connection.          */
/***********************************************************************/
pthread_handler_t ThreadOpen(void *p)
  {
  PTBMT cmp = (PTBMT)p;

  if (!my_thread_init()) {
    set_current_thd(cmp->Thd);

		if (trace(1))
			htrc("ThreadOpen: Thd=%d\n", cmp->Thd);

    // Try to open the connection
		pthread_mutex_lock(&tblmut);

		if (!cmp->Tap->GetTo_Tdb()->OpenDB(cmp->G)) {
//		pthread_mutex_lock(&tblmut);
			if (trace(1))
				htrc("Table %s ready\n", cmp->Tap->GetName());

			cmp->Ready = true;
//		pthread_mutex_unlock(&tblmut);
		} else {
//		pthread_mutex_lock(&tblmut);
			if (trace(1))
				htrc("Opening %s failed\n", cmp->Tap->GetName());

			cmp->Rc = RC_FX;
//		pthread_mutex_unlock(&tblmut);
		}	// endif OpenDB

		pthread_mutex_unlock(&tblmut);
		my_thread_end();
  } else
    cmp->Rc = RC_FX;

  return NULL;
  } // end of ThreadOpen

/***********************************************************************/
/*  TDBTBM constructors.                                               */
/***********************************************************************/
TDBTBM::TDBTBM(PTBLDEF tdp) : TDBTBL(tdp)
  {
  Tmp = NULL;              // To data table TBMT structures
  Cmp = NULL;              // Current data table TBMT
  Bmp = NULL;              // To bad (unconnected) TBMT structures
  Done = false;            // TRUE after first GetAllResults
  Nrc = 0;                 // Number of remote connections
  Nlc = 0;                 // Number of local connections
  } // end of TDBTBL standard constructor

/***********************************************************************/
/*  Reset read/write position values.                                  */
/***********************************************************************/
void TDBTBM::ResetDB(void)
  {
  for (PCOL colp = Columns; colp; colp = colp->GetNext())
    if (colp->GetAmType() == TYPE_AM_TABID)
      colp->COLBLK::Reset();

	// Local tables
  for (PTABLE tabp = Tablist; tabp; tabp = tabp->GetNext())
    tabp->GetTo_Tdb()->ResetDB();

	// Remote tables
	for (PTBMT tp = Tmp; tp; tp = tp->Next)
		tp->Tap->GetTo_Tdb()->ResetDB();

  Tdbp = (Tablist) ? Tablist->GetTo_Tdb() : NULL;
  Crp = 0;
  } // end of ResetDB

/***********************************************************************/
/*  Returns RowId if b is false or Rownum if b is true.                */
/***********************************************************************/
int TDBTBM::RowNumber(PGLOBAL g, bool b)
  {
  return Tdbp->RowNumber(g) + ((b) ? 0 : Rows);
  } // end of RowNumber

/***********************************************************************/
/*  Returns true if this MYSQL table refers to a local table.          */
/***********************************************************************/
bool TDBTBM::IsLocal(PTABLE tbp)
{
	TDBMYSQL *tdbp = (TDBMYSQL*)tbp->GetTo_Tdb();

	return ((!stricmp(tdbp->Host, "localhost") ||
		       !strcmp(tdbp->Host, "127.0.0.1")) &&
                       (int) tdbp->Port == (int)GetDefaultPort());
}	// end of IsLocal

/***********************************************************************/
/*  Initialyze table parallel processing.                              */
/***********************************************************************/
bool TDBTBM::OpenTables(PGLOBAL g)
  {
  int    k;
  THD   *thd = current_thd;
  PTABLE tabp, *ptabp = &Tablist;
  PTBMT  tp, *ptp = &Tmp;

  // Allocates the TBMT blocks for the tables
  for (tabp = Tablist; tabp; tabp = tabp->Next)
    if (tabp->GetTo_Tdb()->GetAmType() == TYPE_AM_MYSQL && !IsLocal(tabp)) {
      // Remove remote table from the local list
      *ptabp = tabp->Next;

			if (trace(1))
				htrc("=====> New remote table %s\n", tabp->GetName());

      // Make the remote table block
      tp = (PTBMT)PlugSubAlloc(g, NULL, sizeof(TBMT));
      memset(tp, 0, sizeof(TBMT));
      tp->G = g;
			tp->Ready = false;
      tp->Tap = tabp;
      tp->Thd = thd;

      // Create the thread that will do the table opening.
      pthread_attr_init(&tp->attr);
//    pthread_attr_setdetachstate(&tp->attr, PTHREAD_CREATE_JOINABLE);

      if ((k = pthread_create(&tp->Tid, &tp->attr, ThreadOpen, tp))) {
        sprintf(g->Message, "pthread_create error %d", k);
        Nbc++;
        continue;
        } // endif k

      // Add it to the remote list
      *ptp = tp;
      ptp = &tp->Next;
      Nrc++;         // Number of remote connections
    } else {
			if (trace(1))
				htrc("=====> Local table %s\n", tabp->GetName());

			ptabp = &tabp->Next;
      Nlc++;         // Number of local connections
    } // endif Type

  return false;
  } // end of OpenTables

/***********************************************************************/
/*  TBL Access Method opening routine.                                 */
/*  Open first file, other will be opened sequencially when reading.   */
/***********************************************************************/
bool TDBTBM::OpenDB(PGLOBAL g)
  {
  if (trace(1))
    htrc("TBM OpenDB: tdbp=%p tdb=R%d use=%d key=%p mode=%d\n",
                      this, Tdb_No, Use, To_Key_Col, Mode);

  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, replace it at its beginning.               */
    /*******************************************************************/
    ResetDB();
    return (Tdbp) ? Tdbp->OpenDB(g) : false;  // Re-open fist table
    } // endif use

#if 0
  /*********************************************************************/
  /*  When GetMaxsize was called, To_CondFil was not set yet.          */
  /*********************************************************************/
  if (To_CondFil && Tablist) {
    Tablist = NULL;
    Nbc = 0;
    } // endif To_CondFil
#endif // 0

  /*********************************************************************/
  /*  Make the table list.                                             */
  /*********************************************************************/
  if (/*!Tablist &&*/ InitTableList(g))
    return TRUE;

  /*********************************************************************/
  /*  Open all remote tables of the list.                              */
  /*********************************************************************/
  if (OpenTables(g))
    return TRUE;

  /*********************************************************************/
  /*  Proceed with local tables.                                       */
  /*********************************************************************/
  if ((CurTable = Tablist)) {
    Tdbp = CurTable->GetTo_Tdb();
//  Tdbp->SetMode(Mode);

    // Check and initialize the subtable columns
    for (PCOL cp = Columns; cp; cp = cp->GetNext())
      if (cp->GetAmType() == TYPE_AM_TABID)
        cp->COLBLK::Reset();
      else if (((PPRXCOL)cp)->Init(g, NULL) && !Accept)
        return TRUE;
        
    if (trace(1))
      htrc("Opening subtable %s\n", Tdbp->GetName());

    // Now we can safely open the table
    if (Tdbp->OpenDB(g))
      return TRUE;

    } // endif *Tablist

  Use = USE_OPEN;
  return FALSE;
  } // end of OpenDB

/***********************************************************************/
/*  ReadDB: Data Base read routine for MUL access method.              */
/***********************************************************************/
int TDBTBM::ReadDB(PGLOBAL g)
  {
  int rc;

  if (!Done) {
    // Get result from local tables
    if ((rc = TDBTBL::ReadDB(g)) != RC_EF)
      return rc;
    else if ((rc = ReadNextRemote(g)) != RC_OK)
      return rc;

    Done = true;
    } // endif Done

  /*********************************************************************/
  /*  Now start the reading process of remote tables.                  */
  /*********************************************************************/
 retry:
  rc = Tdbp->ReadDB(g);

  if (rc == RC_EF) {
    // Total number of rows met so far
    Rows += Tdbp->RowNumber(g) - 1;
    Crp += Tdbp->GetProgMax(g);
    Cmp->Complete = true;

    if ((rc = ReadNextRemote(g)) == RC_OK)
      goto retry;

  } else if (rc == RC_FX)
    strcat(strcat(strcat(g->Message, " ("), Tdbp->GetName()), ")");

  return rc;
  } // end of ReadDB

/***********************************************************************/
/*  ReadNext: Continue reading from next table.                        */
/***********************************************************************/
int TDBTBM::ReadNextRemote(PGLOBAL g)
  {
  bool b;

  if (Tdbp)
    Tdbp->CloseDB(g);

  Cmp = NULL;

 retry:
	b = false;

	// Search for a remote table having its result set
	pthread_mutex_lock(&tblmut);
	for (PTBMT  tp = Tmp; tp; tp = tp->Next)
		if (tp->Rc != RC_FX) {
			if (tp->Ready) {
				if (!tp->Complete) {
					Cmp = tp;
					break;
				}	// endif Complete

			} else
				b = true;

		}	// endif Rc

	pthread_mutex_unlock(&tblmut);

  if (!Cmp) {
    if (b) {          // more result to come
//    sleep(20);
      goto retry;
    } else
      return RC_EF;

    } // endif Curtable

  Tdbp = Cmp->Tap->GetTo_Tdb();

  // Check and initialize the subtable columns
  for (PCOL cp = Columns; cp; cp = cp->GetNext())
    if (cp->GetAmType() == TYPE_AM_TABID)
      cp->COLBLK::Reset();
    else if (((PPRXCOL)cp)->Init(g, NULL) && !Accept)
      return RC_FX;

  if (trace(1))
    htrc("Reading subtable %s\n", Tdbp->GetName());

  return RC_OK;
  } // end of ReadNextRemote
#endif   // DEVELOPMENT

/* ------------------------------------------------------------------- */
