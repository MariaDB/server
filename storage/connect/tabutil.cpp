/************* Tabutil cpp Declares Source Code File (.CPP) ************/
/*  Name: TABUTIL.CPP   Version 1.0                                    */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2013         */
/*                                                                     */
/*  Utility function used by the PROXY, XCOL, OCCUR, and TBL tables.   */
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
extern "C" int zconv;

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
	k = sprintf(key, "%s", db) + 1;
	k += sprintf(key + k, "%s", name);
  key[++k] = 0;

	if (!(s = alloc_table_share(db, name, key, ++k))) {
    strcpy(g->Message, "Error allocating share\n");
    return NULL;
    } // endif s

//        1           2          4            8 
//flags = GTS_TABLE | GTS_VIEW | GTS_NOLOCK | GTS_FORCE_DISCOVERY;

  if (!open_table_def(thd, s, GTS_TABLE | GTS_VIEW)) {
    if (!s->is_view) {
      if (stricmp(plugin_name(s->db_plugin)->str, "connect")) {
#if defined(MYSQL_SUPPORT)
        mysql = true;
#else   // !MYSQL_SUPPORT
        sprintf(g->Message, "%s.%s is not a CONNECT table", db, name);
        return NULL;
#endif   // MYSQL_SUPPORT
      } else
        mysql = false;

    } else {
      mysql = true;
    } // endif is_view

  } else {
    sprintf(g->Message, "Error %d opening share\n", s->error);
    free_table_share(s);
    return NULL;
  } // endif open_table_def

  return s;
} // end of GetTableShare

/************************************************************************/
/*  TabColumns: constructs the result blocks containing all the columns */
/*  description of the object table that will be retrieved by discovery.*/
/************************************************************************/
PQRYRES TabColumns(PGLOBAL g, THD *thd, const char *db, 
                                        const char *name, bool& info)
  {
  int  buftyp[] = {TYPE_STRING, TYPE_SHORT,  TYPE_STRING, TYPE_INT,
                   TYPE_INT,    TYPE_SHORT,  TYPE_SHORT,  TYPE_SHORT,
                   TYPE_STRING, TYPE_STRING, TYPE_STRING};
  XFLD fldtyp[] = {FLD_NAME,   FLD_TYPE,  FLD_TYPENAME, FLD_PREC,
                   FLD_LENGTH, FLD_SCALE, FLD_RADIX,    FLD_NULL,
                   FLD_REM,    FLD_NO,    FLD_CHARSET};
  unsigned int length[] = {0, 4, 16, 4, 4, 4, 4, 4, 0, 32, 32};
  char        *fld, *colname, *chset, *fmt, v;
  int          i, n, ncol = sizeof(buftyp) / sizeof(int);
  int          prec, len, type, scale;
  bool         mysql;
  TABLE_SHARE *s = NULL;
  Field*      *field;
  Field       *fp;
  PQRYRES      qrp;
  PCOLRES      crp;

  if (!info) {
    if (!(s = GetTableShare(g, thd, db, name, mysql))) {
      return NULL;
    } else if (s->is_view) {
      strcpy(g->Message, "Use MYSQL type to see columns from a view");
      info = true;       // To tell caller name is a view
      free_table_share(s);
      return NULL;
    } else
      n = s->fieldnames.count;

  } else {
    n = 0;
    length[0] = 128;
  } // endif info

  /**********************************************************************/
  /*  Allocate the structures used to refer to the result set.          */
  /**********************************************************************/
  if (!(qrp = PlgAllocResult(g, ncol, n, IDS_COLUMNS + 3,
                             buftyp, fldtyp, length, false, true)))
    return NULL;

  // Some columns must be renamed
  for (i = 0, crp = qrp->Colresp; crp; crp = crp->Next)
    switch (++i) {
      case  2: crp->Nulls = (char*)PlugSubAlloc(g, NULL, n); break;
      case 10: crp->Name = "Date_fmt";  break;
      case 11: crp->Name = "Collation"; break;
      } // endswitch i

  if (info)
    return qrp;

  /**********************************************************************/
  /*  Now get the results into blocks.                                  */
  /**********************************************************************/
  for (i = 0, field= s->field; *field; field++) {
    fp= *field;

    // Get column name
    crp = qrp->Colresp;                    // Column_Name
    colname = (char *)fp->field_name;
    crp->Kdata->SetValue(colname, i);

    chset = (char *)fp->charset()->name;
    v = (!strcmp(chset, "binary")) ? 'B' : 0;

    if ((type = MYSQLtoPLG(fp->type(), &v)) == TYPE_ERROR) {
      if (v == 'K') {
        // Skip this column
        sprintf(g->Message, "Column %s skipped (unsupported type)", colname);
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
        continue;
        } // endif v

      sprintf(g->Message, "Column %s unsupported type", colname);
      qrp = NULL;
      break;
      } // endif type

      if (v == 'X') {
        len = zconv;
        sprintf(g->Message, "Column %s converted to varchar(%d)",
                colname, len);
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
        } // endif v

    crp = crp->Next;                       // Data_Type
    crp->Kdata->SetValue(type, i);

    if (fp->flags & ZEROFILL_FLAG)
      crp->Nulls[i] = 'Z';
    else if (fp->flags & UNSIGNED_FLAG)
      crp->Nulls[i] = 'U';
    else                  // X means TEXT field
      crp->Nulls[i] = (v == 'X') ? 'V' : v;

    crp = crp->Next;                       // Type_Name
    crp->Kdata->SetValue(GetTypeName(type), i);
    fmt = NULL;

    if (type == TYPE_DATE) {
      // When creating tables we do need info about date columns
      if (mysql) {
        fmt = MyDateFmt(fp->type());
        prec = len = strlen(fmt);
      } else {
        fmt = (char*)fp->option_struct->dateformat;
        prec = len = fp->field_length;
      } // endif mysql

    } else if (v != 'X') {
      if (type == TYPE_DECIM)
        prec = ((Field_new_decimal*)fp)->precision;
      else
        prec = fp->field_length;
//      prec = (prec(???) == NOT_FIXED_DEC) ? 0 : fp->field_length;

      len = fp->char_length();
    } else
      prec = len = zconv;

    crp = crp->Next;                       // Precision
    crp->Kdata->SetValue(prec, i);

    crp = crp->Next;                       // Length
    crp->Kdata->SetValue(len, i);

    crp = crp->Next;                       // Scale
    scale = (type == TYPE_DOUBLE || type == TYPE_DECIM) ? fp->decimals()
                                                        : 0;
    crp->Kdata->SetValue(scale, i);

    crp = crp->Next;                       // Radix
    crp->Kdata->SetValue(0, i);

    crp = crp->Next;                       // Nullable
    crp->Kdata->SetValue((fp->null_ptr != 0) ? 1 : 0, i);

    crp = crp->Next;                       // Remark

    // For Valgrind
    if (fp->comment.length > 0 && (fld = fp->comment.str))
      crp->Kdata->SetValue(fld, fp->comment.length, i);
    else
      crp->Kdata->Reset(i);

    crp = crp->Next;                       // New (date format)
    crp->Kdata->SetValue((fmt) ? fmt : (char*) "", i);

    crp = crp->Next;                       // New (charset)
    fld = (char *)fp->charset()->name;
    crp->Kdata->SetValue(fld, i);

    // Add this item
    qrp->Nblin++;
    i++;                                   // Can be skipped
    } // endfor field

  /**********************************************************************/
  /*  Return the result pointer for use by GetData routines.            */
  /**********************************************************************/
  if (s)
	  free_table_share(s);
	  
  return qrp;
  } // end of TabColumns

/* -------------- Implementation of the PROXY classes	---------------- */

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
  char *pn, *db, *tab, *def = NULL;

  db = GetStringCatInfo(g, "Dbname", "*");
  def = GetStringCatInfo(g, "Srcdef", NULL);

  if (!(tab = GetStringCatInfo(g, "Tabname", NULL))) {
    if (!def) {
      strcpy(g->Message, "Missing object table definition");
      return TRUE;
    } else
      tab = "Noname";

  } else
    // Analyze the table name, it may have the format: [dbname.]tabname
    if ((pn = strchr(tab, '.'))) {
      *pn++ = 0;
      db = tab;
      tab = pn;
      } // endif pn

  Tablep = new(g) XTAB(tab, def);
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
PTDBASE TDBPRX::GetSubTable(PGLOBAL g, PTABLE tabp, bool b)
  {
  const char  *sp = NULL;
  char        *db, *name;
  bool         mysql = true;
  PTDB         tdbp = NULL;
  TABLE_SHARE *s = NULL;
  Field*      *fp = NULL;
  PCATLG       cat = To_Def->GetCat();
  PHC          hc = ((MYCAT*)cat)->GetHandler();
  LPCSTR       cdb, curdb = hc->GetDBName(NULL);
  THD         *thd = (hc->GetTable())->in_use;

  db = (char*)tabp->GetQualifier();
  name = (char*)tabp->GetName();

  // Check for eventual loop
  for (PTABLE tp = To_Table; tp; tp = tp->Next) {
    cdb = (tp->Qualifier) ? tp->Qualifier : curdb;

    if (!stricmp(name, tp->Name) && !stricmp(db, cdb)) {
      sprintf(g->Message, "Table %s.%s pointing on itself", db, name);
      return NULL;
      } // endif

    } // endfor tp

  if (!tabp->GetSrc()) {
    if (!(s = GetTableShare(g, thd, db, name, mysql)))
      return NULL;

    if (s->is_view && !b)
      s->field = hc->get_table()->s->field;

    hc->tshp = s;
  } else if (b) {
    // Don't use caller's columns
    fp = hc->get_table()->field;
    hc->get_table()->field = NULL;

    // Make caller use the source definition
    sp = hc->get_table()->s->option_struct->srcdef;
    hc->get_table()->s->option_struct->srcdef = tabp->GetSrc();
  } // endif srcdef

  if (mysql) {
#if defined(MYSQL_SUPPORT)
    // Access sub-table via MySQL API
    if (!(tdbp= cat->GetTable(g, tabp, MODE_READ, "MYPRX"))) {
      char buf[MAX_STR];

      strcpy(buf, g->Message);
      sprintf(g->Message, "Error accessing %s.%s: %s", db, name, buf);
      hc->tshp = NULL;
      goto err;
      } // endif Define

    if (db)
      ((PTDBMY)tdbp)->SetDatabase(tabp->GetQualifier());

#else   // !MYSQL_SUPPORT
      sprintf(g->Message, "%s.%s is not a CONNECT table",
                          db, tblp->Name);
      goto err;
#endif   // MYSQL_SUPPORT
  } else {
    // Sub-table is a CONNECT table
    tabp->Next = To_Table;          // For loop checking
    tdbp = cat->GetTable(g, tabp);
  } // endif mysql

  if (s) {
    if (s->is_view && !b)
      s->field = NULL;

    hc->tshp = NULL;
  } else if (b) {
    // Restore s structure that can be in cache
    hc->get_table()->field = fp;
    hc->get_table()->s->option_struct->srcdef = sp;
  } // endif s

  if (trace && tdbp)
    htrc("Subtable %s in %s\n", 
          name, SVP(((PTDBASE)tdbp)->GetDef()->GetDB()));
 
 err:
  if (s)
    free_table_share(s);

  return (PTDBASE)tdbp;
  } // end of GetSubTable

/***********************************************************************/
/*  Initializes the table.                                             */
/***********************************************************************/
bool TDBPRX::InitTable(PGLOBAL g)
  {
  if (!Tdbp) {
    // Get the table description block of this table
    if (!(Tdbp = GetSubTable(g, ((PPRXDEF)To_Def)->Tablep)))
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
      return 0;
  
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
/*  PROXY Access Method opening routine.                               */
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
    return TRUE;
  
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

  Use = USE_OPEN;
	return FALSE;
  } // end of OpenDB

/***********************************************************************/
/*  Data Base read routine for PROY access method.                     */
/***********************************************************************/
int TDBPRX::ReadDB(PGLOBAL g)
  {
  /*********************************************************************/
  /*  Now start the reading process.                                   */
  /*********************************************************************/
	return Tdbp->ReadDB(g);
  } // end of ReadDB

/***********************************************************************/
/*  WriteDB: Data Base write routine for PROXY access methods.         */
/***********************************************************************/
int TDBPRX::WriteDB(PGLOBAL g)
  {
	sprintf(g->Message, "%s tables are read only", To_Def->GetType());
  return RC_FX;
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for PROXY access methods.            */
/***********************************************************************/
int TDBPRX::DeleteDB(PGLOBAL g, int irc)
  {
  sprintf(g->Message, "Delete not enabled for %s tables",
                      To_Def->GetType());
  return RC_FX;
  } // end of DeleteDB

/***********************************************************************/
/*  Used by the TBL tables.                                            */
/***********************************************************************/
void TDBPRX::RemoveNext(PTABLE tp)
  {
  tp->Next = NULL;
  } // end of RemoveNext

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
    // May not have been done elsewhere
    Colp->InitValue(g);        
    To_Val = Colp->GetValue();

    // this may be needed by some tables (which?)
    Colp->SetColUse(ColUse);
  } else {
    sprintf(g->Message, MSG(NO_MATCHING_COL), Name, tdbp->Tdbp->GetName());
    return TRUE;
  } // endif Colp

  return FALSE;
  } // end of Init

/***********************************************************************/
/*  Reset the column descriptor to non evaluated yet.                  */
/***********************************************************************/
void PRXCOL::Reset(void)
  {
  if (Colp)
    Colp->Reset();

  Status &= ~BUF_READ;
  } // end of Reset

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void PRXCOL::ReadColumn(PGLOBAL g)
  {
  if (trace > 1)
    htrc("PRX ReadColumn: name=%s\n", Name);

  if (Colp) {
    Colp->Eval(g);
    Value->SetValue_pval(To_Val);

    // Set null when applicable
    if (Nullable)
      Value->SetNull(Value->IsNull());

    } // endif Colp

  } // end of ReadColumn

/* ---------------------------TDBTBC class --------------------------- */

/***********************************************************************/
/*  TDBTBC class constructor.                                          */
/***********************************************************************/
TDBTBC::TDBTBC(PPRXDEF tdp) : TDBCAT(tdp)
  {
  Db  = (PSZ)tdp->Tablep->GetQualifier();    
  Tab = (PSZ)tdp->Tablep->GetName();    
  } // end of TDBTBC constructor

/***********************************************************************/
/*  GetResult: Get the list the MYSQL table columns.                   */
/***********************************************************************/
PQRYRES TDBTBC::GetResult(PGLOBAL g)
  {
  bool b = false;

  return TabColumns(g, current_thd, Db, Tab, b);
	} // end of GetResult

