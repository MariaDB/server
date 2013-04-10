/************* TabTbl C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: TABTBL                                                */
/* -------------                                                       */
/*  Version 1.4                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to PlugDB Software Development          2008-2013    */
/*  Author: Olivier BERTRAND                                           */
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
#if defined(WIN32)
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
#include "table.h"       // MySQL table definitions
#include "global.h"      // global declarations
#include "plgdbsem.h"    // DB application declarations
#include "reldef.h"      // DB definition declares
//#include "filter.h"      // FILTER classes dcls
#include "filamtxt.h"
#include "tabcol.h"
#include "tabdos.h"      // TDBDOS and DOSCOL class dcls
#include "tabtbl.h"      // TDBTBL and TBLCOL classes dcls
#include "ha_connect.h"
#include "mycat.h"       // For GetHandler

extern "C" int trace;

int open_table_def(THD *thd, TABLE_SHARE *share, uint db_flags);

/* ---------------------------- Class TBLDEF ---------------------------- */

/**************************************************************************/
/*  Constructor.                                                          */
/**************************************************************************/
TBLDEF::TBLDEF(void)
  {
  To_Tables = NULL;
  Ntables = 0;
  Pseudo = 3;
  } // end of TBLDEF constructor

/**************************************************************************/
/*  DefineAM: define specific AM block values from XDB file.              */
/**************************************************************************/
bool TBLDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
  {
  char   *tablist, *dbname;

  Desc = "Table list table";
  tablist = Cat->GetStringCatInfo(g, "Tablist", "");
  dbname = Cat->GetStringCatInfo(g, "Database", NULL);
  Ntables = 0;

  if (*tablist) {
    char *p, *pn, *pdb;
    PTBL *ptbl = &To_Tables, tbl;

    for (pdb = tablist; ;) {
      if ((p = strchr(pdb, ',')))
        *p = 0;

      // Analyze the table name, it has the format:
      // [dbname.]tabname
      if ((pn = strchr(pdb, '.'))) {
        *pn++ = 0;
      } else {
        pn = pdb;
        pdb = dbname;
      } // endif p

      // Allocate the TBLIST block for that table
      tbl = (PTBL)PlugSubAlloc(g, NULL, sizeof(TBLIST));
      tbl->Next = NULL;
      tbl->Name = pn;
      tbl->DB = pdb;
      
      if (trace)
        htrc("TBL: Name=%s db=%s\n", tbl->Name, SVP(tbl->DB));

      // Link the blocks
      *ptbl = tbl;
      ptbl = &tbl->Next;
      Ntables++;

      if (p)
        pdb = pn + strlen(pn) + 1;
      else
        break;

      } // endfor pdb

    Maxerr = Cat->GetIntCatInfo("Maxerr", 0);
    Accept = (Cat->GetBoolCatInfo("Accept", 0) != 0);
    } // endif fsec || tablist

  return FALSE;
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB TBLDEF::GetTable(PGLOBAL g, MODE m)
  {
  PTDB tdbp;

  /*********************************************************************/
  /*  Allocate a TDB of the proper type.                               */
  /*  Column blocks will be allocated only when needed.                */
  /*********************************************************************/
  tdbp = new(g) TDBTBL(this);

  return tdbp;
  } // end of GetTable

/* ------------------------- Class TDBTBL ---------------------------- */

/***********************************************************************/
/*  TDBTBL constructors.                                               */
/***********************************************************************/
TDBTBL::TDBTBL(PTBLDEF tdp) : TDBASE(tdp)
  {
  Tablist = NULL;
  CurTable = NULL;
  Tdbp = NULL;
  Accept = tdp->Accept;
  Maxerr = tdp->Maxerr;
  Nbf = 0;
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
  return new(g) TBLCOL(cdp, this, cprec, n);
  } // end of MakeCol

/***********************************************************************/
/*  InsertSpecialColumn: Put a special column ahead of the column list.*/
/***********************************************************************/
PCOL TDBTBL::InsertSpecialColumn(PGLOBAL g, PCOL scp)
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
/*  Get the PTDB of a table of the list.                               */
/***********************************************************************/
PTDB TDBTBL::GetSubTable(PGLOBAL g, PTBL tblp, PTABLE tabp)
  {
  char        *db, key[256];
  uint         k, flags;
  PTDB         tdbp = NULL;
  TABLE_LIST   table_list;
  TABLE_SHARE *s;
  PCATLG       cat = To_Def->GetCat();
  PHC           hc = ((MYCAT*)cat)->GetHandler();
  THD          *thd = (hc->GetTable())->in_use;

  if (!thd)
    return NULL;         // Should not happen anymore

  if (tblp->DB)
    db = tblp->DB;
  else
    db = (char*)hc->GetDBName(NULL);

  table_list.init_one_table(db, strlen(db),
                            tblp->Name, strlen(tblp->Name),
                            NULL, TL_IGNORE);
	k = sprintf(key, "%s", db);
	k += sprintf(key + ++k, "%s", tblp->Name);
  key[++k] = 0;

	if (!(s = alloc_table_share(&table_list, key, ++k))) {
    strcpy(g->Message, "Error allocating share\n");
    return NULL;
    } // endif s

//        1          8                  16
//flags = READ_ALL | DONT_OPEN_TABLES | DONT_OPEN_MASTER_REG;
//flags = 25;
  flags = 24;

  if (!open_table_def(thd, s, flags)) {
    hc->tshp = s;
    tdbp = cat->GetTable(g, tabp);
    hc->tshp = NULL;
  } else
    sprintf(g->Message, "Error %d opening share\n", s->error);

  if (trace && tdbp)
    htrc("Subtable %s in %s\n", 
          tblp->Name, SVP(((PTDBASE)tdbp)->GetDef()->GetDB()));
    
  free_table_share(s);
  return tdbp;
  } // end of GetSubTable

/***********************************************************************/
/*  Initializes the table table list.                                  */
/***********************************************************************/
bool TDBTBL::InitTableList(PGLOBAL g)
  {
  char   *colname;
  int     n, colpos;
  PTBL    tblp;
  PTABLE  tabp;
  PTDB    tdbp;
  PCOL    colp;
  PTBLDEF tdp = (PTBLDEF)To_Def;

//  PlugSetPath(filename, Tdbp->GetFile(g), Tdbp->GetPath());

  for (n = 0, tblp = tdp->GetTables(); tblp; tblp = tblp->Next) {
    if (TestFil(g, To_Filter, tblp)) {
      // Table or named view
      tabp = new(g) XTAB(tblp->Name);
      tabp->SetQualifier(tblp->DB);

      // Get the table description block of this table
      if (!(tdbp = GetSubTable(g, tblp, tabp))) {
        if (++Nbf > Maxerr)
          return TRUE;               // Error return
        else
          continue;                  // Skip this table

          } // endif tdbp

      // We must allocate subtable columns before GetMaxSize is called
      // because some (PLG, ODBC?) need to have their columns attached.
      // Real initialization will be done later.
      for (PCOL cp = Columns; cp; cp = cp->GetNext())
        if (!cp->IsSpecial()) {
          colname = cp->GetName();
          colpos = ((PTBLCOL)cp)->Colnum;

          // We try first to get the column by name
          if (!(colp = tdbp->ColDB(g, colname, 0)) && colpos)
            // When unsuccessful, if a column number was specified
            // try to get the column by its position in the table
            colp = tdbp->ColDB(g, NULL, colpos);

          if (!colp) {
            if (!Accept) {
              sprintf(g->Message, MSG(NO_MATCHING_COL),
                      colname, tdbp->GetName());
              return TRUE;               // Error return
              } // endif !Accept

          } else // this is needed in particular by PLG tables
            colp->SetColUse(cp->GetColUse());

          } // endif !special

      if (Tablist)
        Tablist->Link(tabp);
      else
        Tablist = tabp;

      n++;
      } // endif filp

    } // endfor tblp

//NumTables = n;
  To_Filter = NULL;        // To avoid doing it several times
  return FALSE;
  } // end of InitTableList

/***********************************************************************/
/*  Test the tablename against the pseudo "local" filter.              */
/***********************************************************************/
bool TDBTBL::TestFil(PGLOBAL g, PFIL filp, PTBL tblp)
  {
  char *fil, op[8], tn[NAME_LEN];
  bool  neg;

  if (!filp)
    return TRUE;
  else if (strstr(filp, " OR ") || strstr(filp, " AND "))
    return TRUE;               // Not handled yet
  else
    fil = filp + (*filp == '(' ? 1 : 0);

  if (sscanf(fil, "TABID %s", op) != 1)
    return TRUE;               // ignore invalid filter

  if ((neg = !strcmp(op, "NOT")))
    strcpy(op, "IN");

  if (!strcmp(op, "=")) {
    // Temporarily, filter must be "TABID = 'value'" only
    if (sscanf(fil, "TABID = '%[^']'", tn) != 1)
      return TRUE;             // ignore invalid filter

    return !stricmp(tn, tblp->Name);
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
      else if (!stricmp(tn, tblp->Name))
        return !neg;           // Found

      tnl = p;
      } // endwhile

    return neg;                // Not found
  } // endif op

  return TRUE;                 // invalid operator
  } // end of TestFil

/***********************************************************************/
/*  TBL GetProgMax: get the max value for progress information.        */
/***********************************************************************/
int TDBTBL::GetProgMax(PGLOBAL g)
  {
  PTABLE tblp;
  int   n, pmx = 0;

  if (!Tablist && InitTableList(g))
    return -1;

  for (tblp = Tablist; tblp; tblp = tblp->GetNext())
    if ((n = tblp->GetTo_Tdb()->GetProgMax(g)) > 0)
      pmx += n;

  return pmx;
  } // end of GetProgMax

/***********************************************************************/
/*  TBL GetProgCur: get the current value for progress information.    */
/***********************************************************************/
int TDBTBL::GetProgCur(void)
  {
  return Crp + Tdbp->GetProgCur();
  } // end of GetProgCur

#if 0
/***********************************************************************/
/*  TBL Cardinality: returns table cardinality in number of rows.      */
/*  This function can be called with a null argument to test the       */
/*  availability of Cardinality implementation (1 yes, 0 no).          */
/*  Can be used on Multiple FIX table only.                            */
/***********************************************************************/
int TDBTBL::Cardinality(PGLOBAL g)
  {
  if (!g)
    return Tdbp->Cardinality(g);

  if (!Tablist && InitTableList(g))
    return -1;

  int n, card = 0;

  for (int i = 0; i < NumFiles; i++) {
    Tdbp->SetFile(g, Filenames[i]);
    Tdbp->ResetSize();

    if ((n = Tdbp->Cardinality(g)) < 0) {
//    strcpy(g->Message, MSG(BAD_CARDINALITY));
      return -1;
      } // endif n

    card += n;
    } // endfor i

  return card;
  } // end of Cardinality
#endif // 0

/***********************************************************************/
/*  Sum up the sizes of all sub-tables.                                */
/***********************************************************************/
int TDBTBL::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0) {
    PTABLE tblp;
    int    mxsz;

    if (!Tablist && InitTableList(g))
      return 0;               // Cannot be calculated at this stage

//  if (Use == USE_OPEN) {
//    strcpy(g->Message, MSG(MAXSIZE_ERROR));
//    return -1;
//  } else
      MaxSize = 0;

    for (tblp = Tablist; tblp; tblp = tblp->GetNext()) {
      if ((mxsz = tblp->GetTo_Tdb()->GetMaxSize(g)) < 0) {
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
    if (colp->GetAmType() == TYPE_AM_TABID)
      colp->COLBLK::Reset();

  for (PTABLE tblp = Tablist; tblp; tblp = tblp->GetNext())
    ((PTDBASE)tblp->GetTo_Tdb())->ResetDB();

  Tdbp = (PTDBASE)Tablist->GetTo_Tdb();
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
  if (trace)
    htrc("TBL OpenDB: tdbp=%p tdb=R%d use=%d key=%p mode=%d\n",
                      this, Tdb_No, Use, To_Key_Col, Mode);

  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, replace it at its beginning.               */
    /*******************************************************************/
    ResetDB();
    return Tdbp->OpenDB(g);  // Re-open fist table
    } // endif use

#if 0
  /*********************************************************************/
  /*  Direct access needed for join or sorting.                        */
  /*********************************************************************/
  if (NeedIndexing(g)) {
    // Direct access of TBL tables is not implemented yet
    strcpy(g->Message, MSG(NO_MUL_DIR_ACC));
    return TRUE;
    } // endif NeedIndexing
#endif // 0

  /*********************************************************************/
  /*  When GetMaxsize was called, To_Filter was not set yet.           */
  /*********************************************************************/
  if (To_Filter && Tablist) {
    Tablist = NULL;
    Nbf = 0;
    } // endif To_Filter

  /*********************************************************************/
  /*  Open the first table of the list.                                */
  /*********************************************************************/
  if (!Tablist && InitTableList(g))     //  done in GetMaxSize
    return TRUE;

  if ((CurTable = Tablist)) {
    Tdbp = (PTDBASE)CurTable->GetTo_Tdb();
    Tdbp->SetMode(Mode);
//  Tdbp->ResetDB();
//  Tdbp->ResetSize();

    // Check and initialize the subtable columns
    for (PCOL cp = Columns; cp; cp = cp->GetNext())
      if (cp->GetAmType() == TYPE_AM_TABID)
        cp->COLBLK::Reset();
      else if (((PTBLCOL)cp)->Init(g))
        return TRUE;
        
    if (trace)
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
        Tdbp = (PTDBASE)CurTable->GetTo_Tdb();

        // Check and initialize the subtable columns
        for (PCOL cp = Columns; cp; cp = cp->GetNext())
          if (cp->GetAmType() == TYPE_AM_TABID)
            cp->COLBLK::Reset();
          else if (((PTBLCOL)cp)->Init(g))
            return RC_FX;

        if (trace)
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

/***********************************************************************/
/*  Data Base write routine for MUL access method.                     */
/***********************************************************************/
int TDBTBL::WriteDB(PGLOBAL g)
  {
  strcpy(g->Message, MSG(TABMUL_READONLY));
  return RC_FX;                    // NIY
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for MUL access method.               */
/***********************************************************************/
int TDBTBL::DeleteDB(PGLOBAL g, int irc)
  {
  strcpy(g->Message, MSG(TABMUL_READONLY));
  return RC_FX;                                      // NIY
  } // end of DeleteDB

/***********************************************************************/
/*  Data Base close routine for MUL access method.                     */
/***********************************************************************/
void TDBTBL::CloseDB(PGLOBAL g)
  {
  if (Tdbp)
    Tdbp->CloseDB(g);

  } // end of CloseDB

/* ---------------------------- TBLCOL ------------------------------- */

/***********************************************************************/
/*  TBLCOL public constructor.                                         */
/***********************************************************************/
TBLCOL::TBLCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PSZ am)
  : COLBLK(cdp, tdbp, i)
  {
  if (cprec) {
    Next = cprec->GetNext();
    cprec->SetNext(this);
  } else {
    Next = tdbp->GetColumns();
    tdbp->SetColumns(this);
  } // endif cprec

  // Set additional Dos access method information for column.
  Long = cdp->GetLong();           // ???
//strcpy(F_Date, cdp->F_Date);
  Colp = NULL;
  To_Val = NULL;
  Pseudo = FALSE;
  Colnum = cdp->GetOffset();       // If columns are retrieved by number

  if (trace)
    htrc(" making new %sCOL C%d %s at %p\n", am, Index, Name, this);

  } // end of TBLCOL constructor

#if 0
/***********************************************************************/
/*  TBLCOL public constructor.                                         */
/***********************************************************************/
TBLCOL::TBLCOL(SPCBLK *scp, PTDB tdbp) : COLBLK(scp->GetName(), tdbp, 0)
  {
  // Set additional TBL access method information for pseudo column.
  Is_Key = Was_Key = scp->IsKey();
  Long = scp->GetLength();
  Buf_Type = scp->GetResultType();
  *Format.Type = (Buf_Type == TYPE_INT) ? 'N' : 'C';
  Format.Length = Long;
  Colp = NULL;
  To_Val = NULL;
  Pseudo = TRUE;
  } // end of TBLCOL constructor

/***********************************************************************/
/*  TBLCOL constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
TBLCOL::TBLCOL(TBLCOL *col1, PTDB tdbp) : COLBLK(col1, tdbp)
  {
  Long = col1->Long;
  Colp = col1->Colp;
  To_Val = col1->To_Val;
  Pseudo = col1->Pseudo;
  } // end of TBLCOL copy constructor
#endif

/***********************************************************************/
/*  TBLCOL initialization routine.                                     */
/*  Look for the matching column in the current table.                 */
/***********************************************************************/
bool TBLCOL::Init(PGLOBAL g)
  {
  PTDBTBL tdbp = (PTDBTBL)To_Tdb;

  To_Val = NULL;

  if (!(Colp = tdbp->Tdbp->ColDB(g, Name, 0)) && Colnum)
    Colp = tdbp->Tdbp->ColDB(g, NULL, Colnum);

  if (Colp) {
    Colp->InitValue(g);        // May not have been done elsewhere
    To_Val = Colp->GetValue();
  } else if (!tdbp->Accept) {
    sprintf(g->Message, MSG(NO_MATCHING_COL), Name, tdbp->Tdbp->GetName());
    return TRUE;
  } else
    Value->Reset();

  return FALSE;
  } // end of Init

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void TBLCOL::ReadColumn(PGLOBAL g)
  {
  if (trace)
    htrc("TBL ReadColumn: name=%s\n", Name);

  if (Colp) {
    Colp->ReadColumn(g);
    Value->SetValue_pval(To_Val);

    // Set null when applicable
    if (Colp->IsNullable())
      Value->SetNull(Value->IsZero());

    } // endif Colp

  } // end of ReadColumn

/* ---------------------------- TBTBLK ------------------------------- */

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void TBTBLK::ReadColumn(PGLOBAL g)
  {
  if (trace)
    htrc("TBT ReadColumn: name=%s\n", Name);

  Value->SetValue_psz((char*)((PTDBTBL)To_Tdb)->Tdbp->GetName());

  } // end of ReadColumn

/* ------------------------------------------------------------------- */
