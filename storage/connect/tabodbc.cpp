/************* Tabodbc C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: TABODBC                                               */
/* -------------                                                       */
/*  Version 2.5                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2000-2013    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the TABODBC class DB execution routines.          */
/*                                                                     */
/* WHAT YOU NEED TO COMPILE THIS PROGRAM:                              */
/* --------------------------------------                              */
/*                                                                     */
/*  REQUIRED FILES:                                                    */
/*  ---------------                                                    */
/*    TABODBC.CPP    - Source code                                     */
/*    PLGDBSEM.H     - DB application declaration file                 */
/*    TABODBC.H      - TABODBC classes declaration file                */
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
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include "my_global.h"
#if defined(WIN32)
#include <io.h>
#include <fcntl.h>
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif
//#include <windows.h>
#include <sqltypes.h>
#else
#if defined(UNIX)
#include <errno.h>
#define NODW
#include "osutil.h"
#else
#include <io.h>
#endif
#include <fcntl.h>
#endif

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  kindex.h    is kindex header that also includes tabdos.h.          */
/*  tabodbc.h   is header containing the TABODBC class declarations.   */
/*  odbconn.h   is header containing ODBC connection declarations.     */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "mycat.h"
#include "xtable.h"
#include "tabodbc.h"
#include "odbccat.h"
#include "tabmul.h"
#include "reldef.h"
#include "tabcol.h"
#include "valblk.h"

#include "sql_string.h"

extern "C" int   trace;

/***********************************************************************/
/*  DB static variables.                                               */
/***********************************************************************/
//     int num_read, num_there, num_eq[2], num_nf;        // Statistics
extern int num_read, num_there, num_eq[2];                // Statistics

/* -------------------------- Class ODBCDEF -------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
ODBCDEF::ODBCDEF(void)
  {
  Connect = Tabname = Tabowner = Tabqual = Qchar = NULL;
  Catver = Options = 0; 
  }  // end of ODBCDEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values from XDB file.           */
/***********************************************************************/
bool ODBCDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
  {
  int    dop = ODBConn::noOdbcDialog;    // Default for options

  Desc = Connect = Cat->GetStringCatInfo(g, "Connect", "");
  Tabname = Cat->GetStringCatInfo(g, "Name",
                 (Catfunc & (FNC_TABLE | FNC_COL)) ? NULL : Name);
  Tabname = Cat->GetStringCatInfo(g, "Tabname", Tabname);
  Tabowner = Cat->GetStringCatInfo(g, "Owner", "");
  Tabqual = Cat->GetStringCatInfo(g, "Qualifier", "");
  Qchar = Cat->GetStringCatInfo(g, "Qchar", "");
  Catver = Cat->GetIntCatInfo("Catver", 2);
  Options = Cat->GetIntCatInfo("Options", dop);
  Pseudo = 2;    // FILID is Ok but not ROWID
  return false;
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB ODBCDEF::GetTable(PGLOBAL g, MODE m)
  {
  PTDBASE tdbp = NULL;

  /*********************************************************************/
  /*  Allocate a TDB of the proper type.                               */
  /*  Column blocks will be allocated only when needed.                */
  /*********************************************************************/
  switch (Catfunc) {
    case FNC_COL:
      tdbp = new(g) TDBOCL(this);
      break;
    case FNC_TABLE:
      tdbp = new(g) TDBOTB(this);
      break;
    case FNC_DSN:
      tdbp = new(g) TDBSRC(this);
      break;
    case FNC_DRIVER:
      tdbp = new(g) TDBDRV(this);
      break;
    default:
      tdbp = new(g) TDBODBC(this);
  
      if (Multiple == 1)
        tdbp = new(g) TDBMUL(tdbp);
      else if (Multiple == 2)
        strcpy(g->Message, MSG(NO_ODBC_MUL));
    } // endswitch Catfunc

  return tdbp;
  } // end of GetTable

/* -------------------------- Class TDBODBC -------------------------- */

/***********************************************************************/
/*  Implementation of the TDBODBC class.                               */
/***********************************************************************/
TDBODBC::TDBODBC(PODEF tdp) : TDBASE(tdp)
  {
  Ocp = NULL;
  Cnp = NULL;

  if (tdp) {
    Connect = tdp->GetConnect();
    TableName = tdp->GetTabname();
    Owner = tdp->GetTabowner();
    Qualifier = tdp->GetTabqual();
    Quote = tdp->GetQchar();
    Options = tdp->GetOptions();
    Rows = tdp->GetElemt();
    Catver = tdp->GetCatver();
  } else {
    Connect = NULL;
    TableName = NULL;
    Owner = NULL;
    Qualifier = NULL;
    Quote = NULL;
    Options = 0;
    Rows = 0;
    Catver = 0;
  } // endif tdp

  Query = NULL;
  Count = NULL;
//Where = NULL;
  MulConn = NULL;
  DBQ = NULL;
  Fpos = 0;
  AftRows = 0;
  CurNum = 0;
  Rbuf = 0;
  BufSize = 0;
  Nparm = 0;
  } // end of TDBODBC standard constructor

TDBODBC::TDBODBC(PTDBODBC tdbp) : TDBASE(tdbp)
  {
  Ocp = tdbp->Ocp;            // is that right ?
  Cnp = tdbp->Cnp;
  Connect = tdbp->Connect;
  TableName = tdbp->TableName;
  Owner = tdbp->Owner;
  Qualifier = tdbp->Qualifier;
  Quote = tdbp->Quote;
  Query = tdbp->Query;
  Count = tdbp->Count;
//Where = tdbp->Where;
  MulConn = tdbp->MulConn;
  DBQ = tdbp->DBQ;
  Options = tdbp->Options;
  Rows = tdbp->Rows;
  Fpos = tdbp->Fpos;
  AftRows = tdbp->AftRows;
//Tpos = tdbp->Tpos;
//Spos = tdbp->Spos;
  CurNum = tdbp->CurNum;
  Rbuf = tdbp->Rbuf;
  BufSize = tdbp->BufSize;
  Nparm = tdbp->Nparm;
  } // end of TDBODBC copy constructor

// Method
PTDB TDBODBC::CopyOne(PTABS t)
  {
  PTDB     tp;
  PODBCCOL cp1, cp2;
  PGLOBAL  g = t->G;        // Is this really useful ???

  tp = new(g) TDBODBC(this);

  for (cp1 = (PODBCCOL)Columns; cp1; cp1 = (PODBCCOL)cp1->GetNext()) {
    cp2 = new(g) ODBCCOL(cp1, tp);  // Make a copy
    NewPointer(t, cp1, cp2);
    } // endfor cp1

  return tp;
  } // end of CopyOne

/***********************************************************************/
/*  Allocate ODBC column description block.                            */
/***********************************************************************/
PCOL TDBODBC::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  return new(g) ODBCCOL(cdp, this, cprec, n);
  } // end of MakeCol

/***********************************************************************/
/*  Extract the filename from connect string and return it.            */
/*  This used for Multiple(1) tables. Also prepare a connect string    */
/*  with a place holder to be used by SetFile.                         */
/***********************************************************************/
PSZ TDBODBC::GetFile(PGLOBAL g)
  {
  if (Connect) {
    char  *p1, *p2;
    size_t n;

    if ((p1 = strstr(Connect, "DBQ="))) {
      p1 += 4;                        // Beginning of file name
      p2 = strchr(p1, ';');           // End of file path/name

      // Make the File path/name from the connect string
      n = (p2) ? p2 - p1 : strlen(p1);
      DBQ = (PSZ)PlugSubAlloc(g, NULL, n + 1);
      memcpy(DBQ, p1, n);
      DBQ[n] = '\0';

      // Make the Format used to re-generate Connect (3 = "%s" + 1)
      MulConn = (char*)PlugSubAlloc(g, NULL, strlen(Connect) - n + 3);
      memcpy(MulConn, Connect, p1 - Connect);
      MulConn[p1 - Connect] = '\0';
      strcat(strcat(MulConn, "%s"), (p2) ? p2 : ";");
      } // endif p1

    } // endif Connect

  return (DBQ) ? DBQ : (PSZ)"???";
  } // end of GetFile

/***********************************************************************/
/*  Set DBQ and get the new file name into the connect string.         */
/***********************************************************************/
void TDBODBC::SetFile(PGLOBAL g, PSZ fn)
  {
  if (MulConn) {
    int n = strlen(MulConn) + strlen(fn) - 1;

    if (n > BufSize) {
      // Allocate a buffer larger than needed so the chance
      // of having to reallocate it is reduced.
      BufSize = n + 6;
      Connect = (char*)PlugSubAlloc(g, NULL, BufSize);
      } // endif n

    // Make the complete connect string
    sprintf(Connect, MulConn, fn);
    } // endif MultConn

  DBQ = fn;
  } // end of SetFile


/******************************************************************/
/*  Convert an UTF-8 string to latin characters.                  */
/******************************************************************/
int TDBODBC::Decode(char *txt, char *buf, size_t n)
{
  uint dummy_errors;
  uint32 len= copy_and_convert(buf, n, &my_charset_latin1,
                               txt, strlen(txt),
                               &my_charset_utf8_general_ci,
                               &dummy_errors);
  buf[len]= '\0';
  return 0;
} // end of Decode


/***********************************************************************/
/*  MakeSQL: make the SQL statement use with ODBC connection.          */
/*  Note: when implementing EOM filtering, column only used in local   */
/*  filter should be removed from column list.                         */
/***********************************************************************/
char *TDBODBC::MakeSQL(PGLOBAL g, bool cnt)
  {
  char   *colist, *tabname, *sql, buf[64];
  LPCSTR  ownp = NULL, qualp = NULL;
  int     len, ncol = 0;
  bool    first = true;
  PTABLE  tablep = To_Table;
  PCOL    colp;

  if (!cnt) {
    // Normal SQL statement to retrieve results
    for (colp = Columns; colp; colp = colp->GetNext())
      if (!colp->IsSpecial())
        ncol++;

    if (ncol) {
      colist = (char*)PlugSubAlloc(g, NULL, (NAM_LEN + 4) * ncol);

      for (colp = Columns; colp; colp = colp->GetNext())
        if (!colp->IsSpecial()) {
          // Column name can be in UTF-8 encoding
          /*rc=*/ Decode(colp->GetName(), buf, sizeof(buf));

          if (Quote) {
            if (first) {
              strcat(strcat(strcpy(colist, Quote), buf), Quote);
              first = false;
            } else
              strcat(strcat(strcat(strcat(colist, ", "),
                                   Quote), buf), Quote);

          } else {
            if (first) {
              strcpy(colist, buf);
              first = false;
            } else
              strcat(strcat(colist, ", "), buf);

          } // endif Quote

          } // endif !Special

    } else {
      // ncol == 0 can occur for queries such that sql count(*) from...
      // for which we will count the rows from sql * from...
      colist = (char*)PlugSubAlloc(g, NULL, 2);
      strcpy(colist, "*");
    } // endif ncol

  } else {
    // SQL statement used to retrieve the size of the result
    colist = (char*)PlugSubAlloc(g, NULL, 9);
    strcpy(colist, "count(*)");
  } // endif cnt

  // Table name can be encoded in UTF-8
  /*rc = */Decode(TableName, buf, sizeof(buf));

  // Put table name between identifier quotes in case in contains blanks
  tabname = (char*)PlugSubAlloc(g, NULL, strlen(buf) + 3);

  if (Quote)
    strcat(strcat(strcpy(tabname, Quote), buf), Quote);
  else
    strcpy(tabname, buf);

  // Below 14 is length of 'select ' + length of ' from ' + 1
  len = (strlen(colist) + strlen(buf) + 14);
  len += (To_Filter ? strlen(To_Filter) + 7 : 0);

//  if (tablep->GetQualifier())             This is used when using a table
//    qualp = tablep->GetQualifier();       from anotherPlugDB database but
//  else                                    makes no sense for ODBC.
  if (Qualifier && *Qualifier)
    qualp = Qualifier;

  if (qualp)
    len += (strlen(qualp) + 2);

  if (tablep->GetCreator())
    ownp = tablep->GetCreator();
  else if (Owner && *Owner)
    ownp = Owner;

  if (ownp)
    len += (strlen(ownp) + 1);

  sql = (char*)PlugSubAlloc(g, NULL, len);
  strcat(strcat(strcpy(sql, "SELECT "), colist), " FROM ");

  if (qualp) {
    strcat(sql, qualp);

    if (ownp)
      strcat(strcat(sql, "."), ownp);
    else
      strcat(sql, ".");

    strcat(sql, ".");
  } else if (ownp)
    strcat(strcat(sql, ownp), ".");

  strcat(sql, tabname);

  if (To_Filter)
    strcat(strcat(sql, " WHERE "), To_Filter);

  return sql;
  } // end of MakeSQL

/***********************************************************************/
/*  ResetSize: call by TDBMUL when calculating size estimate.          */
/***********************************************************************/
void TDBODBC::ResetSize(void)
  {
  MaxSize = -1;

  if (Ocp && Ocp->IsOpen())
    Ocp->Close();

  } // end of ResetSize

/***********************************************************************/
/*  ODBC GetMaxSize: returns table size estimate in number of lines.   */
/***********************************************************************/
int TDBODBC::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0) {
    if (!Ocp)
      Ocp = new(g) ODBConn(g, this);

    if (!Ocp->IsOpen())
      if (Ocp->Open(Connect, Options) < 1)
        return -1;

    if (!Count && !(Count = MakeSQL(g, true)))
      return -2;

    if (!Cnp) {
      // Allocate a Count(*) column (must not use the default constructor)
      Cnp = new(g) ODBCCOL;
      Cnp->InitValue(g);
      } // endif Cnp

    if ((MaxSize = Ocp->GetResultSize(Count, Cnp)) < 0)
      return -3;

    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  Return 0 in mode DELETE or UPDATE to tell that it is done.         */
/***********************************************************************/
int TDBODBC::GetProgMax(PGLOBAL g)
  {
  return (Mode == MODE_DELETE || Mode == MODE_UPDATE) ? 0
                                                      : GetMaxSize(g);
  } // end of GetProgMax

/***********************************************************************/
/*  ODBC Access Method opening routine.                                */
/*  New method now that this routine is called recursively (last table */
/*  first in reverse order): index blocks are immediately linked to    */
/*  join block of next table if it exists or else are discarted.       */
/***********************************************************************/
bool TDBODBC::OpenDB(PGLOBAL g)
  {
  bool rc = false;

  if (g->Trace)
    htrc("ODBC OpenDB: tdbp=%p tdb=R%d use=%dmode=%d\n",
            this, Tdb_No, Use, Mode);

  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, just replace it at its beginning.          */
    /*******************************************************************/
//  if (To_Kindex)
      /*****************************************************************/
      /*  Table is to be accessed through a sorted index table.        */
      /*****************************************************************/
//    To_Kindex->Reset();

//  rewind(Stream);    >>>>>>> Something to be done with Cursor <<<<<<<
    return false;
    } // endif use

  /*********************************************************************/
  /*  Open an ODBC connection for this table.                          */
  /*  Note: this may not be the proper way to do. Perhaps it is better */
  /*  to test whether a connection is already open for this datasource */
  /*  and if so to allocate just a new result set. But this only for   */
  /*  drivers allowing concurency in getting results ???               */
  /*********************************************************************/
  if (!Ocp)
    Ocp = new(g) ODBConn(g, this);
  else if (Ocp->IsOpen())
    Ocp->Close();

  if (Ocp->Open(Connect, Options) < 1)
    return true;

  Use = USE_OPEN;       // Do it now in case we are recursively called

  /*********************************************************************/
  /*  Allocate whatever is used for getting results.                   */
  /*********************************************************************/
  if (Mode == MODE_READ) {
    /*******************************************************************/
    /* The issue here is that if max result size is needed, it must be */
    /* calculated before the result set for the final data retrieval is*/
    /* allocated and the final statement prepared so we call GetMaxSize*/
    /* here. It can be a waste of time if the max size is not needed   */
    /* but currently we always are asking for it (for progress info).  */
    /*******************************************************************/
    GetMaxSize(g);        // Will be set for next call

    if (!Query)
      if ((Query = MakeSQL(g, false))) {
        for (PODBCCOL colp = (PODBCCOL)Columns;
                colp; colp = (PODBCCOL)colp->GetNext())
          if (!colp->IsSpecial())
            colp->AllocateBuffers(g, Rows);

      } else
        rc = true;

    if (!rc)
      rc = ((Rows = Ocp->ExecDirectSQL(Query, (PODBCCOL)Columns)) < 0);

  } else {
    strcpy(g->Message, "ODBC tables are read only in this version");
    return true;
  } // endelse

  if (rc) {
    Ocp->Close();
    return true;
    } // endif rc

  /*********************************************************************/
  /*  Reset statistics values.                                         */
  /*********************************************************************/
  num_read = num_there = num_eq[0] = num_eq[1] = 0;
  return false;
  } // end of OpenDB

/***********************************************************************/
/*  GetRecpos: return the position of last read record.                */
/***********************************************************************/
int TDBODBC::GetRecpos(void)
  {
  return Fpos;              // To be really implemented
  } // end of GetRecpos

/***********************************************************************/
/*  VRDNDOS: Data Base read routine for odbc access method.            */
/***********************************************************************/
int TDBODBC::ReadDB(PGLOBAL g)
  {
  int   rc;

  if (trace > 1)
    htrc("ODBC ReadDB: R%d Mode=%d key=%p link=%p Kindex=%p\n",
      GetTdb_No(), Mode, To_Key_Col, To_Link, To_Kindex);

  if (To_Kindex) {
    // Direct access of ODBC tables is not implemented yet
    strcpy(g->Message, MSG(NO_ODBC_DIRECT));
    longjmp(g->jumper[g->jump_level], GetAmType());

#if 0
    /*******************************************************************/
    /*  Reading is by an index table.                                  */
    /*******************************************************************/
    int recpos = To_Kindex->Fetch(g);

    switch (recpos) {
      case -1:           // End of file reached
        return RC_EF;
      case -2:           // No match for join
        return RC_NF;
      case -3:           // Same record as current one
        num_there++;
        return RC_OK;
      default:
        /***************************************************************/
        /*  Set the cursor position according to record to read.       */
        /***************************************************************/
//--------------------------------- TODO --------------------------------
        break;
      } // endswitch recpos
#endif // 0

    } // endif To_Kindex

  /*********************************************************************/
  /*  Now start the reading process.                                   */
  /*  Here is the place to fetch the line(s).                          */
  /*********************************************************************/
  if (++CurNum >= Rbuf) {
    Rbuf = Ocp->Fetch();
    CurNum = 0;
    } // endif CurNum

  rc = (Rbuf > 0) ? RC_OK : (Rbuf == 0) ? RC_EF : RC_FX;
  Fpos++;                // Used for progress info

  if (trace > 1)
    htrc(" Read: Rbuf=%d rc=%d\n", Rbuf, rc);

  return rc;
  } // end of ReadDB

/***********************************************************************/
/*  Data Base Insert write routine for ODBC access method.             */
/***********************************************************************/
int TDBODBC::WriteDB(PGLOBAL g)
  {
  strcpy(g->Message, "ODBC tables are read only");
  return RC_FX;
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for ODBC access method.              */
/***********************************************************************/
int TDBODBC::DeleteDB(PGLOBAL g, int irc)
  {
  strcpy(g->Message, MSG(NO_ODBC_DELETE));
  return RC_FX;
  } // end of DeleteDB

/***********************************************************************/
/*  Data Base close routine for ODBC access method.                    */
/***********************************************************************/
void TDBODBC::CloseDB(PGLOBAL g)
  {
//if (To_Kindex) {
//  To_Kindex->Close();
//  To_Kindex = NULL;
//  } // endif

  Ocp->Close();

  if (trace)
    htrc("ODBC CloseDB: closing %s\n", Name);

  } // end of CloseDB

/* --------------------------- ODBCCOL ------------------------------- */

/***********************************************************************/
/*  ODBCCOL public constructor.                                        */
/***********************************************************************/
ODBCCOL::ODBCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PSZ am)
       : COLBLK(cdp, tdbp, i)
  {
  if (cprec) {
    Next = cprec->GetNext();
    cprec->SetNext(this);
  } else {
    Next = tdbp->GetColumns();
    tdbp->SetColumns(this);
  } // endif cprec

  // Set additional ODBC access method information for column.
  Long = cdp->GetLong();
//strcpy(F_Date, cdp->F_Date);
  To_Val = NULL;
  Slen = 0;
  StrLen = &Slen;
  Sqlbuf = NULL;
  Bufp = NULL;
  Blkp = NULL;
  Rank = 0;           // Not known yet

  if (trace)
    htrc(" making new %sCOL C%d %s at %p\n", am, Index, Name, this);

  } // end of ODBCCOL constructor

/***********************************************************************/
/*  ODBCCOL private constructor.                                       */
/***********************************************************************/
ODBCCOL::ODBCCOL(void) : COLBLK()
  {
  Buf_Type = TYPE_INT;     // This is a count(*) column
  // Set additional Dos access method information for column.
  Long = sizeof(int);
  To_Val = NULL;
  Slen = 0;
  StrLen = &Slen;
  Sqlbuf = NULL;
  Bufp = NULL;
  Blkp = NULL;
  Rank = 1;
  } // end of ODBCCOL constructor

/***********************************************************************/
/*  ODBCCOL constructor used for copying columns.                      */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
ODBCCOL::ODBCCOL(ODBCCOL *col1, PTDB tdbp) : COLBLK(col1, tdbp)
  {
  Long = col1->Long;
//strcpy(F_Date, col1->F_Date);
  To_Val = col1->To_Val;
  Slen = col1->Slen;
  StrLen = col1->StrLen;
  Sqlbuf = col1->Sqlbuf;
  Bufp = col1->Bufp;
  Blkp = col1->Blkp;
  Rank = col1->Rank;
  } // end of ODBCCOL copy constructor

/***********************************************************************/
/*  SetBuffer: prepare a column block for write operation.             */
/***********************************************************************/
bool ODBCCOL::SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check)
  {
  if (!(To_Val = value)) {
    sprintf(g->Message, MSG(VALUE_ERROR), Name);
    return true;
  } else if (Buf_Type == value->GetType()) {
    // Values are of the (good) column type
    if (Buf_Type == TYPE_DATE) {
      // If any of the date values is formatted
      // output format must be set for the receiving table
      if (GetDomain() || ((DTVAL *)value)->IsFormatted())
        goto newval;          // This will make a new value;

    } else if (Buf_Type == TYPE_FLOAT)
      // Float values must be written with the correct (column) precision
      // Note: maybe this should be forced by ShowValue instead of this ?
      value->SetPrec(GetPrecision());

    Value = value;            // Directly access the external value
  } else {
    // Values are not of the (good) column type
    if (check) {
      sprintf(g->Message, MSG(TYPE_VALUE_ERR), Name,
              GetTypeName(Buf_Type), GetTypeName(value->GetType()));
      return true;
      } // endif check

 newval:
    if (InitValue(g))         // Allocate the matching value block
      return true;

  } // endif's Value, Buf_Type

  // Because Colblk's have been made from a copy of the original TDB in
  // case of Update, we must reset them to point to the original one.
  if (To_Tdb->GetOrig())
    To_Tdb = (PTDB)To_Tdb->GetOrig();

  // Set the Column
  Status = (ok) ? BUF_EMPTY : BUF_NO;
  return false;
  } // end of SetBuffer

/***********************************************************************/
/*  ReadColumn: when SQLFetch is used there is nothing to do as the    */
/*  column buffer was bind to the record set. This is also the case    */
/*  when calculating MaxSize (Bufp is NULL even when Rows is not).     */
/***********************************************************************/
void ODBCCOL::ReadColumn(PGLOBAL g)
  {
  PTDBODBC tdbp = (PTDBODBC)To_Tdb;
  int n = tdbp->CurNum;

  if (StrLen[n] == SQL_NULL_DATA) {
    // Null value
    if (Nullable)
      Value->SetNull(true);

    Value->Reset();
    return;
  } else
    Value->SetNull(false);

  if (Bufp && tdbp->Rows)
    if (Buf_Type == TYPE_DATE)
      *Sqlbuf = ((TIMESTAMP_STRUCT*)Bufp)[n];
    else
      Value->SetValue_pvblk(Blkp, n);

  if (Buf_Type == TYPE_DATE) {
    struct tm dbtime = {0,0,0,0,0,0,0,0,0};

    dbtime.tm_sec = (int)Sqlbuf->second;
    dbtime.tm_min = (int)Sqlbuf->minute;
    dbtime.tm_hour = (int)Sqlbuf->hour;
    dbtime.tm_mday = (int)Sqlbuf->day;
    dbtime.tm_mon = (int)Sqlbuf->month - 1;
    dbtime.tm_year = (int)Sqlbuf->year - 1900;
    ((DTVAL*)Value)->MakeTime(&dbtime);
    } // endif Buf_Type

  if (g->Trace) {
    char buf[32];

    htrc("ODBC Column %s: rows=%d buf=%p type=%d value=%s\n",
      Name, tdbp->Rows, Bufp, Buf_Type, Value->GetCharString(buf));
    } // endif Trace

  } // end of ReadColumn

/***********************************************************************/
/*  AllocateBuffers: allocate the extended buffer for SQLExtendedFetch */
/*  or Fetch.  Note: we use Long+1 here because ODBC must have space   */
/*  for the ending null character.                                     */
/***********************************************************************/
void ODBCCOL::AllocateBuffers(PGLOBAL g, int rows)
  {
  if (Buf_Type == TYPE_DATE)
    Sqlbuf = (TIMESTAMP_STRUCT*)PlugSubAlloc(g, NULL,
                                             sizeof(TIMESTAMP_STRUCT));

  if (!rows)
    return;

  if (Buf_Type == TYPE_DATE)
    Bufp = PlugSubAlloc(g, NULL, rows * sizeof(TIMESTAMP_STRUCT));
  else {
    Blkp = AllocValBlock(g, NULL, Buf_Type, rows, Long+1, 0, true, false);
    Bufp = Blkp->GetValPointer();
    } // endelse

  if (rows > 1)
    StrLen = (SQLLEN *)PlugSubAlloc(g, NULL, rows * sizeof(int));

  } // end of AllocateBuffers

/***********************************************************************/
/*  Returns the buffer to use for Fetch or Extended Fetch.             */
/***********************************************************************/
void *ODBCCOL::GetBuffer(DWORD rows)
  {
  if (rows && To_Tdb) {
    assert(rows == (DWORD)((TDBODBC*)To_Tdb)->Rows);
    return Bufp;
  } else
    return (Buf_Type == TYPE_DATE) ? Sqlbuf : Value->GetTo_Val();

  } // end of GetBuffer

/***********************************************************************/
/*  Returns the buffer length to use for Fetch or Extended Fetch.      */
/***********************************************************************/
SWORD ODBCCOL::GetBuflen(void)
  {
  if (Buf_Type == TYPE_DATE)
    return (SWORD)sizeof(TIMESTAMP_STRUCT);
  else if (Buf_Type == TYPE_STRING)
    return (SWORD)Value->GetClen() + 1;
  else
    return (SWORD)Value->GetClen();

  } // end of GetBuflen

/***********************************************************************/
/*  WriteColumn: make sure the bind buffer is updated.                 */
/***********************************************************************/
void ODBCCOL::WriteColumn(PGLOBAL g)
  {
  /*********************************************************************/
  /*  Do convert the column value if necessary.                        */
  /*********************************************************************/
  if (Value != To_Val)
    Value->SetValue_pval(To_Val, false);   // Convert the inserted value

  if (Buf_Type == TYPE_DATE) {
    struct tm tm, *dbtime = ((DTVAL*)Value)->GetGmTime(&tm);

    Sqlbuf->second = dbtime->tm_sec;
    Sqlbuf->minute = dbtime->tm_min;
    Sqlbuf->hour   = dbtime->tm_hour;
    Sqlbuf->day    = dbtime->tm_mday;
    Sqlbuf->month  = dbtime->tm_mon + 1;
    Sqlbuf->year   = dbtime->tm_year + 1900;
    } // endif Buf_Type

  } // end of WriteColumn

/* ---------------------------TDBSRC class --------------------------- */

/***********************************************************************/
/*  GetResult: Get the list of ODBC data sources.                      */
/***********************************************************************/
PQRYRES TDBSRC::GetResult(PGLOBAL g)
  {
  return ODBCDataSources(g, false);
	} // end of GetResult

/* ---------------------------TDBDRV class --------------------------- */

/***********************************************************************/
/*  GetResult: Get the list of ODBC drivers.                           */
/***********************************************************************/
PQRYRES TDBDRV::GetResult(PGLOBAL g)
  {
  return ODBCDrivers(g, false);
	} // end of GetResult

/* ---------------------------TDBOTB class --------------------------- */

/***********************************************************************/
/*  TDBOTB class constructor.                                          */
/***********************************************************************/
TDBOTB::TDBOTB(PODEF tdp) : TDBCAT(tdp)
  {
  Dsn = tdp->GetConnect(); 
  Tab = tdp->GetTabname();
  } // end of TDBOTB constructor

/***********************************************************************/
/*  GetResult: Get the list of ODBC tables.                            */
/***********************************************************************/
PQRYRES TDBOTB::GetResult(PGLOBAL g)
  {
  return ODBCTables(g, Dsn, Tab, false);
	} // end of GetResult

/* ---------------------------TDBOCL class --------------------------- */

/***********************************************************************/
/*  GetResult: Get the list of ODBC table columns.                     */
/***********************************************************************/
PQRYRES TDBOCL::GetResult(PGLOBAL g)
  {
  return ODBCColumns(g, Dsn, Tab, NULL, false);
	} // end of GetResult

/* ------------------------ End of Tabodbc --------------------------- */
