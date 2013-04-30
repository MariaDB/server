/************* Tabutil cpp Declares Source Code File (.H) **************/
/*  Name: TABUTIL.CPP   Version 1.0                                    */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2013         */
/*                                                                     */
/*  Utility function used by TBL and PRX tables.                       */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant section of system dependant header files.         */
/***********************************************************************/
#include "my_global.h"
#include "sql_class.h"
#include "table.h"
#include "field.h"
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
#include "global.h"
#include "plgdbsem.h"
#include "plgcnx.h"                       // For DB types
#include "myutil.h"
#include "mycat.h"
#include "valblk.h"
#include "resource.h"
#include "reldef.h"
#include "xtable.h"
#if defined(MYSQL_SUPPORT)
#include "tabmysql.h"
#endif   // MYSQL_SUPPORT
#include "tabcol.h"
#include "tabutil.h"
#include "ha_connect.h"

extern "C" int trace;

/************************************************************************/
/*  Used by MYSQL tables to get MySQL parameters from the calling proxy */
/*  table (PROXY, TBL, XCL, or OCCUR) when used by one of these.        */
/************************************************************************/
void Remove_tshp(PCATLG cat)
{
  ((MYCAT*)cat)->GetHandler()->tshp = NULL;
} // end of Remove_thsp

/************************************************************************/
/*  GetTableShare: allocates and open a table share.                    */
/************************************************************************/
TABLE_SHARE *GetTableShare(PGLOBAL g, THD *thd, const char *db, 
                                      const char *name, bool& mysql)
{
  char         key[256];
  uint         k;
//TABLE_LIST   table_list;
  TABLE_SHARE *s;

//table_list.init_one_table(db, strlen(db), name, strlen(name),
//                          NULL, TL_IGNORE);
	k = sprintf(key, "%s", db);
	k += sprintf(key + ++k, "%s", name);
  key[++k] = 0;

	if (!(s = alloc_table_share(db, name, key, ++k))) {
    strcpy(g->Message, "Error allocating share\n");
    return NULL;
    } // endif s

//        1           2          4            8 
//flags = GTS_TABLE | GTS_VIEW | GTS_NOLOCK | GTS_FORCE_DISCOVERY;

  if (!open_table_def(thd, s, GTS_TABLE)) {
#ifdef DBUG_OFF
    if (stricmp(s->db_plugin->name.str, "connect")) {
#else
    if (stricmp((*s->db_plugin)->name.str, "connect")) {
#endif
#if defined(MYSQL_SUPPORT)
      mysql = true;
#else   // !MYSQL_SUPPORT
      sprintf(g->Message, "%s.%s is not a CONNECT table", db, name);
      return NULL;
#endif   // MYSQL_SUPPORT
    } else
      mysql = false;

  } else {
    sprintf(g->Message, "Error %d opening share\n", s->error);
    free_table_share(s);
    return NULL;
  } // endif open_table_def

  return s;
} // end of GetTableShare

/************************************************************************/
/*  TabColumns: constructs the result blocks containing all the columns */
/*  of the object table that will be retrieved by GetData commands.     */
/*  key = TRUE when called from Create Table to get key informations.   */
/************************************************************************/
PQRYRES TabColumns(PGLOBAL g, THD *thd, const char *db, 
                                        const char *name, bool info)
  {
  static int dbtype[]  = {DB_CHAR, DB_SHORT, DB_CHAR,  DB_INT,
                          DB_INT,  DB_SHORT, DB_SHORT, DB_SHORT,
                          DB_CHAR, DB_CHAR,  DB_CHAR};
  static int buftyp[]  = {TYPE_STRING, TYPE_SHORT,  TYPE_STRING, TYPE_INT,
                          TYPE_INT,    TYPE_SHORT,  TYPE_SHORT,  TYPE_SHORT,
                          TYPE_STRING, TYPE_STRING, TYPE_STRING};
  static XFLD fldtyp[] = {FLD_NAME,   FLD_TYPE,  FLD_TYPENAME, FLD_PREC,
                          FLD_LENGTH, FLD_SCALE, FLD_RADIX,    FLD_NULL,
                          FLD_REM,    FLD_NO,    FLD_CHARSET};
  static unsigned int length[] = {0, 4, 16, 4, 4, 4, 4, 4, 256, 32, 32};
  char        *fld, *fmt;
  int          i, n, ncol = sizeof(dbtype) / sizeof(int);
  int          len, type, prec;
  bool         mysql;
  TABLE_SHARE *s;
  Field*      *field;
  Field       *fp;
  PQRYRES      qrp;
  PCOLRES      crp;

  if (!info) {
    if (!(s = GetTableShare(g, thd, db, name, mysql)))
      return NULL;
    else
      n = s->fieldnames.count;

  } else {
    n = 0;
    length[0] = 128;
  } // endif info

  /**********************************************************************/
  /*  Allocate the structures used to refer to the result set.          */
  /**********************************************************************/
  qrp = PlgAllocResult(g, ncol, n, IDS_COLUMNS + 3,
                          dbtype, buftyp, fldtyp, length, true, true);

  // Some columns must be renamed
  for (i = 0, crp = qrp->Colresp; crp; crp = crp->Next)
    switch (++i) {
      case 10: crp->Name = "Date_fmt";  break;
      case 11: crp->Name = "Collation"; break;
      } // endswitch i

  if (info)
    return qrp;

  /**********************************************************************/
  /*  Now get the results into blocks.                                  */
  /**********************************************************************/
  for (i = 0, field= s->field; *field; i++, field++) {
    fp= *field;

    // Get column name
    crp = qrp->Colresp;                    // Column_Name
    fld = (char *)fp->field_name;
    crp->Kdata->SetValue(fld, i);

    if ((type = MYSQLtoPLG(fp->type())) == TYPE_ERROR) {
      sprintf(g->Message, "Unsupported column type %s", GetTypeName(type));
      qrp = NULL;
      break;
      } // endif type

    crp = crp->Next;                       // Data_Type
    crp->Kdata->SetValue(type, i);
    crp = crp->Next;                       // Type_Name
    crp->Kdata->SetValue(GetTypeName(type), i);

    if (type == TYPE_DATE) {
      // When creating tables we do need info about date columns
      if (mysql) {
        fmt = MyDateFmt(fp->type());
        len = strlen(fmt);
      } else {
        fmt = (char*)fp->option_struct->dateformat;
        len = fp->field_length;
      } // endif mysql

    } else {
      fmt = NULL;
      len = fp->char_length();
    } // endif type

    crp = crp->Next;                       // Precision
    crp->Kdata->SetValue(len, i);

    crp = crp->Next;                       // Length
    len = fp->field_length;
    crp->Kdata->SetValue(len, i);

    prec = (type == TYPE_FLOAT) ? fp->decimals() : 0;
    crp = crp->Next;                       // Scale
    crp->Kdata->SetValue(prec, i);

    crp = crp->Next;                       // Radix
    crp->Kdata->SetValue(0, i);

    crp = crp->Next;                       // Nullable
    crp->Kdata->SetValue((fp->null_ptr != 0) ? 1 : 0, i);

    crp = crp->Next;                       // Remark
    fld = fp->comment.str;
    crp->Kdata->SetValue(fld, fp->comment.length, i);

    crp = crp->Next;                       // New
    crp->Kdata->SetValue((fmt) ? fmt : (char*) "", i);

    crp = crp->Next;                       // New (charset)
    fld = (char *)fp->charset()->name;
    crp->Kdata->SetValue(fld, i);

    // Add this item
    qrp->Nblin++;
    } // endfor field

  /**********************************************************************/
  /*  Return the result pointer for use by GetData routines.            */
  /**********************************************************************/
  free_table_share(s);
  return qrp;
  } // end of TabColumns

/* -------------- Implementation of the XCOL classes	---------------- */

/***********************************************************************/
/*  PRXDEF constructor.                                                */
/***********************************************************************/
PRXDEF::PRXDEF(void)
  {
  Tablep = NULL;
  Pseudo = 3;
} // end of PRXDEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values from XCOL file.          */
/***********************************************************************/
bool PRXDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
  {
  char *pn, *db, *tab;

  db = Cat->GetStringCatInfo(g, "Dbname", "*");
  tab = Cat->GetStringCatInfo(g, "Tabname", NULL);

  // Analyze the table name, it may have the format: [dbname.]tabname
  if ((pn = strchr(tab, '.'))) {
    *pn++ = 0;
    db = tab;
    tab = pn;
    } // endif pn

  Tablep = new(g) XTAB(tab);
  Tablep->SetQualifier(db);
  return FALSE;
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new TDB of the proper type.                      */
/***********************************************************************/
PTDB PRXDEF::GetTable(PGLOBAL g, MODE mode)
  {
  if (Catfunc == FNC_COL)
    return new(g) TDBTBC(this);
  else
    return new(g) TDBPRX(this);

  } // end of GetTable

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Implementation of the TDBPRX class.                                */
/***********************************************************************/
TDBPRX::TDBPRX(PPRXDEF tdp) : TDBASE(tdp)
  {
  Tdbp = NULL;                    // The object table
  } // end of TDBPRX constructor

/***********************************************************************/
/*  Get the PTDB of the sub-table.                                     */
/***********************************************************************/
PTDB TDBPRX::GetSubTable(PGLOBAL g, PTABLE tabp)
  {
  char        *db, *name;
  bool         mysql;
  PTDB         tdbp = NULL;
  TABLE_SHARE *s;
  PCATLG       cat = To_Def->GetCat();
  PHC          hc = ((MYCAT*)cat)->GetHandler();
  THD         *thd = (hc->GetTable())->in_use;

  db = (char*)tabp->GetQualifier();
  name = (char*)tabp->GetName();

  if (!(s = GetTableShare(g, thd, db, name, mysql)))
    return NULL;

  hc->tshp = s;

  if (mysql) {
#if defined(MYSQL_SUPPORT)
    // Access sub-table via MySQL API
    if (!(tdbp= cat->GetTable(g, tabp, MODE_READ, "MYPRX"))) {
      sprintf(g->Message, "Cannot access %s.%s", db, name);
      goto err;
      } // endif Define

    if (db)
      ((PTDBMY)tdbp)->SetDatabase(tabp->GetQualifier());

#else   // !MYSQL_SUPPORT
      sprintf(g->Message, "%s.%s is not a CONNECT table",
                          db, tblp->Name);
      goto err;
#endif   // MYSQL_SUPPORT
  } else
    // Sub-table is a CONNECT table
    tdbp = cat->GetTable(g, tabp);

  hc->tshp = NULL;

  if (trace && tdbp)
    htrc("Subtable %s in %s\n", 
          name, SVP(((PTDBASE)tdbp)->GetDef()->GetDB()));
 
 err:
  free_table_share(s);
  return tdbp;
  } // end of GetSubTable

/***********************************************************************/
/*  Initializes the table.                                             */
/***********************************************************************/
bool TDBPRX::InitTable(PGLOBAL g)
  {
  if (!Tdbp) {
    // Get the table description block of this table
    if (!(Tdbp = (PTDBASE)GetSubTable(g, ((PPRXDEF)To_Def)->Tablep)))
      return TRUE;

    } // endif Tdbp

  return FALSE;
  } // end of InitTable

/***********************************************************************/
/*  Allocate PRX column description block.                             */
/***********************************************************************/
PCOL TDBPRX::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  return new(g) PRXCOL(cdp, this, cprec, n);
  } // end of MakeCol

/***********************************************************************/
/*  PRX GetMaxSize: returns the maximum number of rows in the table.   */
/***********************************************************************/
int TDBPRX::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0) {
    if (InitTable(g))
      return NULL;
  
  	MaxSize = Tdbp->GetMaxSize(g);
    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  In this sample, ROWID will be the (virtual) row number,            */
/*  while ROWNUM will be the occurence rank in the multiple column.    */
/***********************************************************************/
int TDBPRX::RowNumber(PGLOBAL g, bool b)
	{
	return Tdbp->RowNumber(g, b);
	} // end of RowNumber
 
/***********************************************************************/
/*  XCV Access Method opening routine.                                 */
/***********************************************************************/
bool TDBPRX::OpenDB(PGLOBAL g)
  {
  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, just replace it at its beginning.          */
    /*******************************************************************/
		return Tdbp->OpenDB(g);
    } // endif use

  if (Mode != MODE_READ) {
    /*******************************************************************/
    /* Currently XCOL tables cannot be modified.                       */
    /*******************************************************************/
    strcpy(g->Message, "PROXY tables are read only");
    return TRUE;
    } // endif Mode

  if (InitTable(g))
    return NULL;
  
  /*********************************************************************/
  /*  Check and initialize the subtable columns.                       */
  /*********************************************************************/
  for (PCOL cp = Columns; cp; cp = cp->GetNext())
    if (((PPRXCOL)cp)->Init(g))
      return TRUE;

  /*********************************************************************/
  /*  Physically open the object table.                                */
  /*********************************************************************/
	if (Tdbp->OpenDB(g))
		return TRUE;

	return FALSE;
  } // end of OpenDB

/***********************************************************************/
/*  Data Base read routine for XCV access method.                      */
/***********************************************************************/
int TDBPRX::ReadDB(PGLOBAL g)
  {
  /*********************************************************************/
  /*  Now start the reading process.                                   */
  /*********************************************************************/
	return Tdbp->ReadDB(g);
  } // end of ReadDB

/***********************************************************************/
/*  WriteDB: Data Base write routine for XCV access methods.           */
/***********************************************************************/
int TDBPRX::WriteDB(PGLOBAL g)
  {
	sprintf(g->Message, "%s tables are read only", To_Def->GetType());
  return RC_FX;
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for XCV access methods.              */
/***********************************************************************/
int TDBPRX::DeleteDB(PGLOBAL g, int irc)
  {
  sprintf(g->Message, "Delete not enabled for %s tables",
                      To_Def->GetType());
  return RC_FX;
  } // end of DeleteDB

/* ---------------------------- PRXCOL ------------------------------- */

/***********************************************************************/
/*  PRXCOL public constructor.                                         */
/***********************************************************************/
PRXCOL::PRXCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PSZ am)
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
  Long = cdp->GetLong();         // Useful ???
//strcpy(F_Date, cdp->F_Date);
  Colp = NULL;
  To_Val = NULL;
  Pseudo = FALSE;
  Colnum = cdp->GetOffset();     // If columns are retrieved by number

  if (trace)
    htrc(" making new %sCOL C%d %s at %p\n", am, Index, Name, this);

  } // end of PRXCOL constructor

/***********************************************************************/
/*  PRXCOL initialization routine.                                     */
/*  Look for the matching column in the object table.                  */
/***********************************************************************/
bool PRXCOL::Init(PGLOBAL g)
  {
  PTDBPRX tdbp = (PTDBPRX)To_Tdb;

  if (!(Colp = tdbp->Tdbp->ColDB(g, Name, 0)) && Colnum)
    Colp = tdbp->Tdbp->ColDB(g, NULL, Colnum);

  if (Colp) {
    Colp->InitValue(g);        // May not have been done elsewhere
    To_Val = Colp->GetValue();
  } else {
    sprintf(g->Message, MSG(NO_MATCHING_COL), Name, tdbp->Tdbp->GetName());
    return TRUE;
  } // endif Colp

  return FALSE;
  } // end of Init

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void PRXCOL::ReadColumn(PGLOBAL g)
  {
  if (trace)
    htrc("PRX ReadColumn: name=%s\n", Name);

  if (Colp) {
    Colp->ReadColumn(g);
    Value->SetValue_pval(To_Val);

    // Set null when applicable
    if (Nullable)
      Value->SetNull(Value->IsNull());

    } // endif Colp

  } // end of ReadColumn

#if 0
/* ---------------------------TBCDEF class --------------------------- */

/***********************************************************************/
/*  DefineAM: define specific AM block values from CATLG table.        */
/***********************************************************************/
bool TBCDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
  {
  Desc = "Catalog Table";
  Database = Cat->GetStringCatInfo(g, "Database", "*");
  Tabname = Cat->GetStringCatInfo(g, "Tabname", Tabname);
  return FALSE;
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new TDB of the proper type.                      */
/***********************************************************************/
PTDB TBCDEF::GetTable(PGLOBAL g, MODE m)
  {
  return new(g) TDBTBC(this);
  } // end of GetTable
#endif // 0

/* ---------------------------TDBTBC class --------------------------- */

/***********************************************************************/
/*  TDBTBC class constructor.                                          */
/***********************************************************************/
TDBTBC::TDBTBC(PPRXDEF tdp) : TDBCAT(tdp)
  {
//  Db  = tdp->Database;    
//  Tab = tdp->Tabname;    
  Db  = (PSZ)tdp->Tablep->GetQualifier();    
  Tab = (PSZ)tdp->Tablep->GetName();    
  } // end of TDBTBC constructor

#if 0
/***********************************************************************/
/*  TDBTBC class constructor from TBL table.                           */
/***********************************************************************/
TDBTBC::TDBTBC(PTBLDEF tdp) : TDBCAT(tdp)
  {
  Db  = tdp->To_Tables->DB;
  Tab = tdp->To_Tables->Name;
  } // end of TDBTBC constructor

/***********************************************************************/
/*  TDBTBC class constructor from PRX table.                           */
/***********************************************************************/
TDBTBC::TDBTBC(PXCLDEF tdp) : TDBCAT(tdp)
  {
  Db  = (PSZ)tdp->Tablep->GetQualifier();    
  Tab = (PSZ)tdp->Tablep->GetName();    
  } // end of TDBTBC constructor
#endif // 0

/***********************************************************************/
/*  GetResult: Get the list the MYSQL table columns.                   */
/***********************************************************************/
PQRYRES TDBTBC::GetResult(PGLOBAL g)
  {
  return TabColumns(g, current_thd, Db, Tab, false);
	} // end of GetResult

