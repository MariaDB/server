/************* Tabodbc C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: TABODBC                                               */
/* -------------                                                       */
/*  Version 2.8                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2000-2014    */
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
extern     bool  xinfo;

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
  Connect= Tabname= Tabschema= Tabcat= Srcdef= Qchar= Qrystr= Sep= NULL;
  Catver = Options = Quoted = Maxerr = Maxres = 0;
  Xsrc = false;
  }  // end of ODBCDEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values from XDB file.           */
/***********************************************************************/
bool ODBCDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
  {
  Desc = Connect = GetStringCatInfo(g, "Connect", NULL);

  if (!Connect && !Catfunc) {
    sprintf(g->Message, "Missing connection for ODBC table %s", Name);
    return true;
    } // endif Connect

  Tabname = GetStringCatInfo(g, "Name",
                 (Catfunc & (FNC_TABLE | FNC_COL)) ? NULL : Name);
  Tabname = GetStringCatInfo(g, "Tabname", Tabname);
  Tabschema = GetStringCatInfo(g, "Dbname", NULL);
  Tabschema = GetStringCatInfo(g, "Schema", Tabschema);
  Tabcat = GetStringCatInfo(g, "Qualifier", NULL);
  Tabcat = GetStringCatInfo(g, "Catalog", Tabcat);

  if ((Srcdef = GetStringCatInfo(g, "Srcdef", NULL)))
    Read_Only = true;

  Qrystr = GetStringCatInfo(g, "Query_String", "?");
  Sep = GetStringCatInfo(g, "Separator", NULL);
  Catver = GetIntCatInfo("Catver", 2);
  Xsrc = GetBoolCatInfo("Execsrc", FALSE);
  Maxerr = GetIntCatInfo("Maxerr", 0);
  Maxres = GetIntCatInfo("Maxres", 0);
  Quoted = GetIntCatInfo("Quoted", 0);
  Options = ODBConn::noOdbcDialog;
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
  if (Xsrc)
    tdbp = new(g) TDBXDBC(this);
  else switch (Catfunc) {
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
    Connect = tdp->Connect;
    TableName = tdp->Tabname;
    Schema = tdp->Tabschema;
    Catalog = tdp->Tabcat;
    Srcdef = tdp->Srcdef;
    Qrystr = tdp->Qrystr;
    Sep = tdp->GetSep();
    Options = tdp->Options;
    Quoted = MY_MAX(0, tdp->GetQuoted());
    Rows = tdp->GetElemt();
    Catver = tdp->Catver;
  } else {
    Connect = NULL;
    TableName = NULL;
    Schema = NULL;
    Catalog = NULL;
    Srcdef = NULL;
    Qrystr = NULL;
    Sep = 0;
    Options = 0;
    Quoted = 0;
    Rows = 0;
    Catver = 0;
  } // endif tdp

  Quote = NULL;
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
  Schema = tdbp->Schema;
  Catalog = tdbp->Catalog;
  Srcdef = tdbp->Srcdef;
  Qrystr = tdbp->Qrystr;
  Quote = tdbp->Quote;
  Query = tdbp->Query;
  Count = tdbp->Count;
//Where = tdbp->Where;
  MulConn = tdbp->MulConn;
  DBQ = tdbp->DBQ;
  Options = tdbp->Options;
  Quoted = tdbp->Quoted;
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
  LPCSTR  schmp = NULL, catp = NULL;
  int     len, ncol = 0;
  bool    first = true;
  PTABLE  tablep = To_Table;
  PCOL    colp;

  if (Srcdef)
    return Srcdef;

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
  len += (To_CondFil ? strlen(To_CondFil->Body) + 7 : 0);

  if (Catalog && *Catalog)
    catp = Catalog;

  if (catp)
    len += (strlen(catp) + 2);

  if (tablep->GetSchema())
    schmp = tablep->GetSchema();
  else if (Schema && *Schema)
    schmp = Schema;

  if (schmp)
    len += (strlen(schmp) + 1);

  sql = (char*)PlugSubAlloc(g, NULL, len);
  strcat(strcat(strcpy(sql, "SELECT "), colist), " FROM ");

  if (catp) {
    strcat(sql, catp);

    if (schmp)
      strcat(strcat(sql, "."), schmp);
    else
      strcat(sql, ".");

    strcat(sql, ".");
  } else if (schmp)
    strcat(strcat(sql, schmp), ".");

  strcat(sql, tabname);

  if (To_CondFil)
    strcat(strcat(sql, " WHERE "), To_CondFil->Body);

  return sql;
  } // end of MakeSQL

/***********************************************************************/
/*  MakeInsert: make the Insert statement used with ODBC connection.   */
/***********************************************************************/
char *TDBODBC::MakeInsert(PGLOBAL g)
  {
  char *stmt, *colist, *valist;
//  char *tk = "`";
  int   len = 0;
  bool  b = FALSE;
  PCOL  colp;

  for (colp = Columns; colp; colp = colp->GetNext())
    if (colp->IsSpecial()) {
      strcpy(g->Message, MSG(NO_ODBC_SPECOL));
      return NULL;
    } else {
      len += (strlen(colp->GetName()) + 4);
      ((PODBCCOL)colp)->Rank = ++Nparm;
    } // endif colp

  colist = (char*)PlugSubAlloc(g, NULL, len);
  *colist = '\0';
  valist = (char*)PlugSubAlloc(g, NULL, 2 * Nparm);
  *valist = '\0';

  for (colp = Columns; colp; colp = colp->GetNext()) {
    if (b) {
      strcat(colist, ", ");
      strcat(valist, ",");
    } else
      b = true;

    if (Quote)
      strcat(strcat(strcat(colist, Quote), colp->GetName()), Quote);
    else
      strcat(colist, colp->GetName());

    strcat(valist, "?");        // Parameter marker
    } // endfor colp

  // Below 32 is enough to contain the fixed part of the query
  len = (strlen(TableName) + strlen(colist) + strlen(valist) + 32);
  stmt = (char*)PlugSubAlloc(g, NULL, len);
  strcpy(stmt, "INSERT INTO ");

  if (Quote)
    strcat(strcat(strcat(stmt, Quote), TableName), Quote);
  else
    strcat(stmt, TableName);

  strcat(strcat(strcat(stmt, " ("), colist), ") VALUES (");
  strcat(strcat(stmt, valist), ")");

  return stmt;
  } // end of MakeInsert

/***********************************************************************/
/*  ODBC Bind Parameter function.                                      */
/***********************************************************************/
bool TDBODBC::BindParameters(PGLOBAL g)
  {
  PODBCCOL colp;

  for (colp = (PODBCCOL)Columns; colp; colp = (PODBCCOL)colp->Next) {
    colp->AllocateBuffers(g, 0);

    if (Ocp->BindParam(colp))
      return true;

    } // endfor colp

  return false;
  } // end of BindParameters

/***********************************************************************/
/*  MakeCommand: make the Update or Delete statement to send to the    */
/*  MySQL server. Limited to remote values and filtering.              */
/***********************************************************************/
char *TDBODBC::MakeCommand(PGLOBAL g)
  {
  char *p, name[68], *qc = Ocp->GetQuoteChar();
  char *stmt = (char*)PlugSubAlloc(g, NULL, strlen(Qrystr) + 64);
  char *qrystr = (char*)PlugSubAlloc(g, NULL, strlen(Qrystr) + 1);
  bool  qtd = Quoted > 0;
  int   i = 0, k = 0;

  // Make a lower case copy of the originale query and change
  // back ticks to the data source identifier quoting character
  do {
    qrystr[i] = (Qrystr[i] == '`') ? *qc : tolower(Qrystr[i]);
    } while (Qrystr[i++]);

  // Check whether the table name is equal to a keyword
  // If so, it must be quoted in the original query
  strlwr(strcat(strcat(strcpy(name, " "), Name), " "));

  if (!strstr(" update delete low_priority ignore quick from ", name))
    strlwr(strcpy(name, Name));     // Not a keyword
  else
    strlwr(strcat(strcat(strcpy(name, qc), Name), qc));

  if ((p = strstr(qrystr, name))) {
    for (i = 0; i < p - qrystr; i++)
      stmt[i] = (Qrystr[i] == '`') ? *qc : Qrystr[i];

    stmt[i] = 0;
    k = i + (int)strlen(Name);

    if (qtd && *(p-1) == ' ')
      strcat(strcat(strcat(stmt, qc), TableName), qc);
    else
      strcat(stmt, TableName);

    i = (int)strlen(stmt);

    do {
      stmt[i++] = (Qrystr[k] == '`') ? *qc : Qrystr[k];
      } while (Qrystr[k++]);

  } else {
    sprintf(g->Message, "Cannot use this %s command",
                 (Mode == MODE_UPDATE) ? "UPDATE" : "DELETE");
    return NULL;
  } // endif p

  return stmt;
  } // end of MakeCommand

#if 0
/***********************************************************************/
/*  MakeUpdate: make the SQL statement to send to ODBC connection.     */
/***********************************************************************/
char *TDBODBC::MakeUpdate(PGLOBAL g)
  {
  char *qc, *stmt = NULL, cmd[8], tab[96], end[1024];

  stmt = (char*)PlugSubAlloc(g, NULL, strlen(Qrystr) + 64);
  memset(end, 0, sizeof(end));

  if (sscanf(Qrystr, "%s `%[^`]`%1023c", cmd, tab, end) > 2 ||
      sscanf(Qrystr, "%s \"%[^\"]\"%1023c", cmd, tab, end) > 2)
    qc = Ocp->GetQuoteChar();
  else if (sscanf(Qrystr, "%s %s%1023c", cmd, tab, end) > 2)
    qc = (Quoted) ? Quote : "";
  else {
    strcpy(g->Message, "Cannot use this UPDATE command");
    return NULL;
  } // endif sscanf

  assert(!stricmp(cmd, "update"));
  strcat(strcat(strcat(strcpy(stmt, "UPDATE "), qc), TableName), qc);

  for (int i = 0; end[i]; i++)
    if (end[i] == '`')
      end[i] = *qc;

  strcat(stmt, end);
  return stmt;
  } // end of MakeUpdate

/***********************************************************************/
/*  MakeDelete: make the SQL statement to send to ODBC connection.     */
/***********************************************************************/
char *TDBODBC::MakeDelete(PGLOBAL g)
  {
  char *qc, *stmt = NULL, cmd[8], from[8], tab[96], end[512];

  stmt = (char*)PlugSubAlloc(g, NULL, strlen(Qrystr) + 64);
  memset(end, 0, sizeof(end));

  if (sscanf(Qrystr, "%s %s `%[^`]`%511c", cmd, from, tab, end) > 2 ||
      sscanf(Qrystr, "%s %s \"%[^\"]\"%511c", cmd, from, tab, end) > 2)
    qc = Ocp->GetQuoteChar();
  else if (sscanf(Qrystr, "%s %s %s%511c", cmd, from, tab, end) > 2)
    qc = (Quoted) ? Quote : "";
  else {
    strcpy(g->Message, "Cannot use this DELETE command");
    return NULL;
  } // endif sscanf

  assert(!stricmp(cmd, "delete") && !stricmp(from, "from"));
  strcat(strcat(strcat(strcpy(stmt, "DELETE FROM "), qc), TableName), qc);

  if (*end) {
    for (int i = 0; end[i]; i++)
      if (end[i] == '`')
        end[i] = *qc;

    strcat(stmt, end);
    } // endif end

  return stmt;
  } // end of MakeDelete
#endif // 0

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
/*  ODBC Cardinality: returns table size in number of rows.            */
/***********************************************************************/
int TDBODBC::Cardinality(PGLOBAL g)
  {
  if (!g)
    return (Mode == MODE_ANY && !Srcdef) ? 1 : 0;

  if (Cardinal < 0 && Mode == MODE_ANY && !Srcdef && xinfo) {
    // Info command, we must return the exact table row number
    char     qry[96], tbn[64];
    ODBConn *ocp = new(g) ODBConn(g, this);

    if (ocp->Open(Connect, Options) < 1)
      return -1;

    // Table name can be encoded in UTF-8
    Decode(TableName, tbn, sizeof(tbn));
    strcpy(qry, "SELECT COUNT(*) FROM ");

    if (Quote)
      strcat(strcat(strcat(qry, Quote), tbn), Quote);
    else
      strcat(qry, tbn);

    // Allocate a Count(*) column (must not use the default constructor)
    Cnp = new(g) ODBCCOL;
    Cnp->InitValue(g);

    if ((Cardinal = ocp->GetResultSize(qry, Cnp)) < 0)
      return -3;

    ocp->Close();
  } else
    Cardinal = 10;    // To make MySQL happy

  return Cardinal;
  } // end of Cardinality

/***********************************************************************/
/*  ODBC GetMaxSize: returns table size estimate in number of lines.   */
/***********************************************************************/
int TDBODBC::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0) {
    if (Mode == MODE_DELETE)
      // Return 0 in mode DELETE in case of delete all.
      MaxSize = 0;
    else if (!Cardinality(NULL))
      MaxSize = 10;   // To make MySQL happy
    else if ((MaxSize = Cardinality(g)) < 0)
      MaxSize = 12;   // So we can see an error occured

    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  Return max size value.                                             */
/***********************************************************************/
int TDBODBC::GetProgMax(PGLOBAL g)
  {
  return GetMaxSize(g);
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
  else if (Quoted)
    Quote = Ocp->GetQuoteChar();

  Use = USE_OPEN;       // Do it now in case we are recursively called

  /*********************************************************************/
  /*  Make the command and allocate whatever is used for getting results.                   */
  /*********************************************************************/
  if (Mode == MODE_READ || Mode == MODE_READX) {
    if ((Query = MakeSQL(g, false))) {
      for (PODBCCOL colp = (PODBCCOL)Columns; colp;
                    colp = (PODBCCOL)colp->GetNext())
        if (!colp->IsSpecial())
          colp->AllocateBuffers(g, Rows);

      rc = ((Rows = Ocp->ExecDirectSQL(Query, (PODBCCOL)Columns)) < 0);
      } // endif Query

  } else if (Mode == MODE_INSERT) {
    if ((Query = MakeInsert(g))) {
      if (Nparm != Ocp->PrepareSQL(Query)) {
        strcpy(g->Message, MSG(PARM_CNT_MISS));
        rc = true;
      } else
        rc = BindParameters(g);

      } // endif Query

  } else if (Mode == MODE_UPDATE || Mode == MODE_DELETE)
    Query = MakeCommand(g);
  else
    sprintf(g->Message, "Invalid mode %d", Mode);

  if (!Query || rc) {
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

  if (Mode == MODE_UPDATE || Mode == MODE_DELETE) {
    // Send the UPDATE/DELETE command to the remote table
    if (!Ocp->ExecSQLcommand(Query)) {
      sprintf(g->Message, "%s: %d affected rows", TableName, AftRows);

      if (trace)
        htrc("%s\n", g->Message);

      PushWarning(g, this, 0);    // 0 means a Note
      return RC_EF;               // Nothing else to do
    } else
      return RC_FX;               // Error

    } // endif Mode

  if (To_Kindex) {
    // Direct access of ODBC tables is not implemented yet
    strcpy(g->Message, MSG(NO_ODBC_DIRECT));
    longjmp(g->jumper[g->jump_level], GetAmType());
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
  int n = Ocp->ExecuteSQL();

  if (n < 0) {
    AftRows = n;
    return RC_FX;
  } else
    AftRows += n;

  return RC_OK;
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for ODBC access method.              */
/***********************************************************************/
int TDBODBC::DeleteDB(PGLOBAL g, int irc)
  {
  if (irc == RC_FX) {
    // Send the DELETE (all) command to the remote table
    if (!Ocp->ExecSQLcommand(Query)) {
      sprintf(g->Message, "%s: %d affected rows", TableName, AftRows);

      if (trace)
        htrc("%s\n", g->Message);

      PushWarning(g, this, 0);    // 0 means a Note
      return RC_OK;               // This is a delete all
    } else
      return RC_FX;               // Error

  } else
    return RC_OK;                 // Ignore

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

  if (Ocp)

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
//Long = cdp->GetLong();
  Long = Precision;
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

    } else if (Buf_Type == TYPE_DOUBLE)
      // Float values must be written with the correct (column) precision
      // Note: maybe this should be forced by ShowValue instead of this ?
      value->SetPrec(GetScale());

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

  if (Bufp && tdbp->Rows) {
    if (Buf_Type == TYPE_DATE)
      *Sqlbuf = ((TIMESTAMP_STRUCT*)Bufp)[n];
    else
      Value->SetValue_pvblk(Blkp, n);

    } // endif Bufp

  if (Buf_Type == TYPE_DATE) {
    struct tm dbtime = {0,0,0,0,0,0,0,0,0};

    dbtime.tm_sec = (int)Sqlbuf->second;
    dbtime.tm_min = (int)Sqlbuf->minute;
    dbtime.tm_hour = (int)Sqlbuf->hour;
    dbtime.tm_mday = (int)Sqlbuf->day;
    dbtime.tm_mon = (int)Sqlbuf->month - 1;
    dbtime.tm_year = (int)Sqlbuf->year - 1900;
    ((DTVAL*)Value)->MakeTime(&dbtime);
  } else if (Buf_Type == TYPE_DECIM && tdbp->Sep) {
    // Be sure to use decimal point
    char *p = strchr(Value->GetCharValue(), tdbp->Sep);

    if (p)
      *p = '.';

  } // endif Buf_Type

  if (g->Trace) {
    char buf[64];

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
    Blkp = AllocValBlock(g, NULL, Buf_Type, rows, GetBuflen(), 
                                  GetScale(), true, false, false);
    Bufp = Blkp->GetValPointer();
    } // endelse

  if (rows > 1)
    StrLen = (SQLLEN *)PlugSubAlloc(g, NULL, rows * sizeof(SQLLEN));

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
  SWORD flen;

  switch (Buf_Type) {
    case TYPE_DATE:
      flen = (SWORD)sizeof(TIMESTAMP_STRUCT);
      break;
    case TYPE_STRING:
    case TYPE_DECIM:
      flen = (SWORD)Value->GetClen() + 1;
      break;
    default:
      flen = (SWORD)Value->GetClen();
    } // endswitch Buf_Type

  return flen;
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
    Value->SetValue_pval(To_Val, FALSE);   // Convert the inserted value

  if (Buf_Type == TYPE_DATE) {
    struct tm tm, *dbtime = ((DTVAL*)Value)->GetGmTime(&tm);

    Sqlbuf->second = dbtime->tm_sec;
    Sqlbuf->minute = dbtime->tm_min;
    Sqlbuf->hour   = dbtime->tm_hour;
    Sqlbuf->day    = dbtime->tm_mday;
    Sqlbuf->month  = dbtime->tm_mon + 1;
    Sqlbuf->year   = dbtime->tm_year + 1900;
    Sqlbuf->fraction = 0;
  } else if (Buf_Type == TYPE_DECIM) {
    // Some data sources require local decimal separator
    char *p, sep = ((PTDBODBC)To_Tdb)->Sep;

    if (sep && (p = strchr(Value->GetCharValue(), '.')))
      *p = sep;

  } // endif Buf_Type

  if (Nullable)
    *StrLen = (Value->IsNull()) ? SQL_NULL_DATA :
         (IsTypeChar(Buf_Type)) ? SQL_NTS : 0;

  } // end of WriteColumn

/* -------------------------- Class TDBXDBC -------------------------- */

/***********************************************************************/
/*  Implementation of the TDBODBC class.                               */
/***********************************************************************/
TDBXDBC::TDBXDBC(PODEF tdp) : TDBODBC(tdp)
{
  Cmdlist = NULL;
  Cmdcol = NULL;
  Mxr = tdp->Maxerr;
  Nerr = 0;
} // end of TDBXDBC constructor

TDBXDBC::TDBXDBC(PTDBXDBC tdbp) : TDBODBC(tdbp)
{
  Cmdlist = tdbp->Cmdlist;
  Cmdcol = tdbp->Cmdcol;
  Mxr = tdbp->Mxr;
  Nerr = tdbp->Nerr;
} // end of TDBXDBC copy constructor

PTDB TDBXDBC::CopyOne(PTABS t)
  {
  PTDB     tp;
  PXSRCCOL cp1, cp2;
  PGLOBAL  g = t->G;        // Is this really useful ???

  tp = new(g) TDBXDBC(this);

  for (cp1 = (PXSRCCOL)Columns; cp1; cp1 = (PXSRCCOL)cp1->GetNext()) {
    cp2 = new(g) XSRCCOL(cp1, tp);  // Make a copy
    NewPointer(t, cp1, cp2);
    } // endfor cp1

  return tp;
  } // end of CopyOne

/***********************************************************************/
/*  Allocate XSRC column description block.                            */
/***********************************************************************/
PCOL TDBXDBC::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  PXSRCCOL colp = new(g) XSRCCOL(cdp, this, cprec, n);

  if (!colp->Flag)
    Cmdcol = colp->GetName();

  return colp;
  } // end of MakeCol

/***********************************************************************/
/*  MakeCMD: make the SQL statement to send to ODBC connection.        */
/***********************************************************************/
PCMD TDBXDBC::MakeCMD(PGLOBAL g)
  {
  PCMD xcmd = NULL;

  if (To_CondFil) {
    if (Cmdcol) {
      if (!stricmp(Cmdcol, To_CondFil->Body) &&
          (To_CondFil->Op == OP_EQ || To_CondFil->Op == OP_IN)) {
        xcmd = To_CondFil->Cmds;
      } else
        strcpy(g->Message, "Invalid command specification filter");

    } else
      strcpy(g->Message, "No command column in select list");

  } else if (!Srcdef)
    strcpy(g->Message, "No Srcdef default command");
  else
    xcmd = new(g) CMD(g, Srcdef);

  return xcmd;
  } // end of MakeCMD

#if 0
/***********************************************************************/
/*  ODBC Bind Parameter function.                                      */
/***********************************************************************/
bool TDBXDBC::BindParameters(PGLOBAL g)
  {
  PODBCCOL colp;

  for (colp = (PODBCCOL)Columns; colp; colp = (PODBCCOL)colp->Next) {
    colp->AllocateBuffers(g, 0);

    if (Ocp->BindParam(colp))
      return true;

    } // endfor colp

  return false;
  } // end of BindParameters
#endif // 0

/***********************************************************************/
/*  XDBC GetMaxSize: returns table size (not always one row).          */
/***********************************************************************/
int TDBXDBC::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0)
    MaxSize = 10;             // Just a guess

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  ODBC Access Method opening routine.                                */
/*  New method now that this routine is called recursively (last table */
/*  first in reverse order): index blocks are immediately linked to    */
/*  join block of next table if it exists or else are discarted.       */
/***********************************************************************/
bool TDBXDBC::OpenDB(PGLOBAL g)
  {
  bool rc = false;

  if (g->Trace)
    htrc("ODBC OpenDB: tdbp=%p tdb=R%d use=%dmode=%d\n",
            this, Tdb_No, Use, Mode);

  if (Use == USE_OPEN) {
    strcpy(g->Message, "Multiple execution is not allowed");
    return true;
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

  if (Mode != MODE_READ && Mode != MODE_READX) {
    strcpy(g->Message, "No INSERT/DELETE/UPDATE of XDBC tables");
    return true;
    } // endif Mode

  /*********************************************************************/
  /*  Get the command to execute.                                      */
  /*********************************************************************/
  if (!(Cmdlist = MakeCMD(g))) {
    Ocp->Close();
    return true;
    } // endif Query

  Rows = 1;
  return false;
  } // end of OpenDB

/***********************************************************************/
/*  ReadDB: Data Base read routine for xdbc access method.             */
/***********************************************************************/
int TDBXDBC::ReadDB(PGLOBAL g)
  {
  if (Cmdlist) {
    Query = Cmdlist->Cmd;

    if (Ocp->ExecSQLcommand(Query))
      Nerr++;

    Fpos++;                // Used for progress info
    Cmdlist = (Nerr > Mxr) ? NULL : Cmdlist->Next;
    return RC_OK;
  } else
    return RC_EF;

  } // end of ReadDB

/***********************************************************************/
/*  Data Base delete line routine for ODBC access method.              */
/***********************************************************************/
int TDBXDBC::WriteDB(PGLOBAL g)
  {
  strcpy(g->Message, "Execsrc tables are read only");
  return RC_FX;
  } // end of DeleteDB

/***********************************************************************/
/*  Data Base delete line routine for ODBC access method.              */
/***********************************************************************/
int TDBXDBC::DeleteDB(PGLOBAL g, int irc)
  {
  strcpy(g->Message, MSG(NO_ODBC_DELETE));
  return RC_FX;
  } // end of DeleteDB

/* --------------------------- XSRCCOL ------------------------------- */

/***********************************************************************/
/*  XSRCCOL public constructor.                                        */
/***********************************************************************/
XSRCCOL::XSRCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PSZ am)
       : ODBCCOL(cdp, tdbp, cprec, i, am)
  {
  // Set additional ODBC access method information for column.
  Flag = cdp->GetOffset();
  } // end of XSRCCOL constructor

/***********************************************************************/
/*  XSRCCOL constructor used for copying columns.                      */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
XSRCCOL::XSRCCOL(XSRCCOL *col1, PTDB tdbp) : ODBCCOL(col1, tdbp)
  {
  Flag = col1->Flag;
  } // end of XSRCCOL copy constructor

/***********************************************************************/
/*  ReadColumn: set column value according to Flag.                    */
/***********************************************************************/
void XSRCCOL::ReadColumn(PGLOBAL g)
  {
  PTDBXDBC tdbp = (PTDBXDBC)To_Tdb;

  switch (Flag) {
    case  0: Value->SetValue_psz(tdbp->Query);    break;
    case  1: Value->SetValue(tdbp->AftRows);      break;
    case  2: Value->SetValue_psz(g->Message);     break;
    default: Value->SetValue_psz("Invalid Flag"); break;
    } // endswitch Flag

  } // end of ReadColumn

/***********************************************************************/
/*  WriteColumn: Should never be called.                               */
/***********************************************************************/
void XSRCCOL::WriteColumn(PGLOBAL g)
  {
  // Should never be called
  } // end of WriteColumn

/* ---------------------------TDBDRV class --------------------------- */

/***********************************************************************/
/*  GetResult: Get the list of ODBC drivers.                           */
/***********************************************************************/
PQRYRES TDBDRV::GetResult(PGLOBAL g)
  {
  return ODBCDrivers(g, Maxres, false);
	} // end of GetResult

/* ---------------------------TDBSRC class --------------------------- */

/***********************************************************************/
/*  GetResult: Get the list of ODBC data sources.                      */
/***********************************************************************/
PQRYRES TDBSRC::GetResult(PGLOBAL g)
  {
  return ODBCDataSources(g, Maxres, false);
	} // end of GetResult

/* ---------------------------TDBOTB class --------------------------- */

/***********************************************************************/
/*  TDBOTB class constructor.                                          */
/***********************************************************************/
TDBOTB::TDBOTB(PODEF tdp) : TDBDRV(tdp)
  {
  Dsn = tdp->GetConnect();
  Schema = tdp->GetTabschema();
  Tab = tdp->GetTabname();
  } // end of TDBOTB constructor

/***********************************************************************/
/*  GetResult: Get the list of ODBC tables.                            */
/***********************************************************************/
PQRYRES TDBOTB::GetResult(PGLOBAL g)
  {
  return ODBCTables(g, Dsn, Schema, Tab, Maxres, false);
	} // end of GetResult

/* ---------------------------TDBOCL class --------------------------- */

/***********************************************************************/
/*  GetResult: Get the list of ODBC table columns.                     */
/***********************************************************************/
PQRYRES TDBOCL::GetResult(PGLOBAL g)
  {
  return ODBCColumns(g, Dsn, Schema, Tab, NULL, Maxres, false);
	} // end of GetResult

/* ------------------------ End of Tabodbc --------------------------- */
