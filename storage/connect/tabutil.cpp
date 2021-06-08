/************* Tabutil cpp Declares Source Code File (.CPP) ************/
/*  Name: TABUTIL.CPP   Version 1.2                                    */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2013 - 2017  */
/*                                                                     */
/*  Utility function used by the PROXY, XCOL, OCCUR, and TBL tables.   */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant section of system dependant header files.         */
/***********************************************************************/
#define MYSQL_SERVER 1
#include <my_global.h>
#include "sql_class.h"
#include "table.h"
#include "field.h"
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
#include "table.h"       // MySQL table definitions
#include "global.h"
#include "plgdbsem.h"
#include "plgcnx.h"                       // For DB types
#include "myutil.h"
#include "valblk.h"
#include "resource.h"
//#include "reldef.h"
#include "xtable.h"
#include "tabext.h"
#include "tabmysql.h"
#include "tabcol.h"
#include "tabutil.h"
#include "ha_connect.h"

int GetConvSize(void);

/************************************************************************/
/*  Used by MYSQL tables to get MySQL parameters from the calling proxy */
/*  table (PROXY, TBL, XCL, or OCCUR) when used by one of these.        */
/************************************************************************/
TABLE_SHARE *Remove_tshp(PCATLG cat)
{
  TABLE_SHARE *s = ((MYCAT*)cat)->GetHandler()->tshp;

	((MYCAT*)cat)->GetHandler()->tshp = NULL;
	return s;
} // end of Remove_thsp

/************************************************************************/
/*  Used by MYSQL tables to get MySQL parameters from the calling proxy */
/*  table (PROXY, TBL, XCL, or OCCUR) when used by one of these.        */
/************************************************************************/
void Restore_tshp(PCATLG cat, TABLE_SHARE *s)
{
	((MYCAT*)cat)->GetHandler()->tshp = s;
} // end of Restore_thsp

/************************************************************************/
/*  GetTableShare: allocates and open a table share.                    */
/************************************************************************/
TABLE_SHARE *GetTableShare(PGLOBAL g, THD *thd, const char *db, 
                                      const char *name, bool& mysql)
{
  char         key[256];
  uint         k;
  TABLE_SHARE *s;

	k = sprintf(key, "%s", db) + 1;
	k += sprintf(key + k, "%s", name);
  key[++k] = 0;

	if (!(s = alloc_table_share(db, name, key, ++k))) {
    strcpy(g->Message, "Error allocating share\n");
    return NULL;
    } // endif s

  if (!open_table_def(thd, s, GTS_TABLE | GTS_VIEW)) {
    if (!s->is_view) {
      if (stricmp(plugin_name(s->db_plugin)->str, "connect"))
        mysql = true;
      else
        mysql = false;

    } else
      mysql = true;

  } else {
    if (thd->is_error())
      thd->clear_error();  // Avoid stopping info commands

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
	PCSZ         fmt;
	char        *pn, *tn, *fld, *colname, v; // *chset
  int          i, n, ncol = sizeof(buftyp) / sizeof(int);
  int          prec, len, type, scale;
  int          zconv = GetConvSize();
  bool         mysql;
  TABLE_SHARE *s = NULL;
  Field*      *field;
  Field       *fp;
  PQRYRES      qrp;
  PCOLRES      crp;

  if (!info) {
		// Analyze the table name, it may have the format: [dbname.]tabname
		if (strchr((char*)name, '.')) {
			tn = (char*)PlugDup(g, name);
			pn = strchr(tn, '.');
			*pn++ = 0;
			db = tn;
			name = pn;
		} // endif pn

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

//  chset = (char *)fp->charset()->name;
//  v = (!strcmp(chset, "binary")) ? 'B' : 0;
		v = 0;

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
        fmt = (PCSZ)fp->option_struct->dateformat;
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
bool PRXDEF::DefineAM(PGLOBAL g, LPCSTR, int)
  {
  char *pn, *db, *tab, *def = NULL;

  db = GetStringCatInfo(g, "Dbname", "*");
  def = GetStringCatInfo(g, "Srcdef", NULL);

  if (!(tab = GetStringCatInfo(g, "Tabname", NULL))) {
    if (!def) {
      strcpy(g->Message, "Missing object table definition");
      return true;
    } else
      tab = PlugDup(g, "Noname");

  } else
    // Analyze the table name, it may have the format: [dbname.]tabname
    if ((pn = strchr(tab, '.'))) {
      *pn++ = 0;
      db = tab;
      tab = pn;
      } // endif pn

  Tablep = new(g) XTAB(tab, def);
  Tablep->SetSchema(db);
  return false;
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new TDB of the proper type.                      */
/***********************************************************************/
PTDB PRXDEF::GetTable(PGLOBAL g, MODE)
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

TDBPRX::TDBPRX(PTDBPRX tdbp) : TDBASE(tdbp)
  {
  Tdbp = tdbp->Tdbp;
  } // end of TDBPRX copy constructor

// Method
PTDB TDBPRX::Clone(PTABS t)
  {
  PTDB    tp;
  PPRXCOL cp1, cp2;
  PGLOBAL g = t->G;

  tp = new(g) TDBPRX(this);

  for (cp1 = (PPRXCOL)Columns; cp1; cp1 = (PPRXCOL)cp1->GetNext()) {
    cp2 = new(g) PRXCOL(cp1, tp);  // Make a copy
    NewPointer(t, cp1, cp2);
    } // endfor cp1

  return tp;
  } // end of Clone

/***********************************************************************/
/*  Get the PTDB of the sub-table.                                     */
/***********************************************************************/
PTDB TDBPRX::GetSubTable(PGLOBAL g, PTABLE tabp, bool b)
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

  db = (char*)(tabp->GetSchema() ? tabp->GetSchema() : curdb);
  name = (char*)tabp->GetName();

  // Check for eventual loop
  for (PTABLE tp = To_Table; tp; tp = tp->Next) {
    cdb = (tp->Schema) ? tp->Schema : curdb;

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
    // Access sub-table via MySQL API
    if (!(tdbp= cat->GetTable(g, tabp, Mode, "MYPRX"))) {
      char buf[MAX_STR];

      strcpy(buf, g->Message);
      snprintf(g->Message, MAX_STR, "Error accessing %s.%s: %s", db, name, buf);
      hc->tshp = NULL;
      goto err;
      } // endif Define

    if (db)
      ((PTDBMY)tdbp)->SetDatabase(tabp->GetSchema());

    if (Mode == MODE_UPDATE || Mode == MODE_DELETE)
      tdbp->SetName(Name);      // For Make_Command

  } else {
    // Sub-table is a CONNECT table
    tabp->Next = To_Table;          // For loop checking
    tdbp = cat->GetTable(g, tabp, Mode);
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

  if (trace(1) && tdbp)
    htrc("Subtable %s in %s\n", 
          name, SVP(tdbp->GetDef()->GetDB()));
 
 err:
  if (s)
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
    if (!(Tdbp = GetSubTable(g, ((PPRXDEF)To_Def)->Tablep)))
      return true;

//  Tdbp->SetMode(Mode);
    } // endif Tdbp

  return false;
  } // end of InitTable

/***********************************************************************/
/*  Allocate PRX column description block.                             */
/***********************************************************************/
PCOL TDBPRX::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  return new(g) PRXCOL(cdp, this, cprec, n);
  } // end of MakeCol

/***********************************************************************/
/*  PRX Cardinality: returns the number of rows in the table.          */
/***********************************************************************/
int TDBPRX::Cardinality(PGLOBAL g)
  {
  if (Cardinal < 0) {
    if (InitTable(g))
      return 0;
  
  	Cardinal = Tdbp->Cardinality(g);
    } // endif MaxSize

  return Cardinal;
  } // end of GetMaxSize

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

  if (InitTable(g))
    return true;
  else if (Mode != MODE_READ && (Read_Only || Tdbp->IsReadOnly())) {
    strcpy(g->Message, "Cannot modify a read only table");
    return true;
    } // endif tp
  
  /*********************************************************************/
  /*  Check and initialize the subtable columns.                       */
  /*********************************************************************/
  for (PCOL cp = Columns; cp; cp = cp->GetNext())
    if (((PPRXCOL)cp)->Init(g, Tdbp))
      return true;

  /*********************************************************************/
  /*  In Update mode, the updated column blocks must be distinct from  */
  /*  the read column blocks. So make a copy of the TDB and allocate   */
  /*  its column blocks in mode write (required by XML tables).        */
  /*********************************************************************/
  if (Mode == MODE_UPDATE) {
    PTDB utp;

    if (!(utp= Tdbp->Duplicate(g))) {
      sprintf(g->Message, MSG(INV_UPDT_TABLE), Tdbp->GetName());
      return true;
      } // endif tp

    for (PCOL cp = To_SetCols; cp; cp = cp->GetNext())
      if (((PPRXCOL)cp)->Init(g, utp))
        return true;

  } else if (Mode == MODE_DELETE)
    Tdbp->SetNext(Next);

  /*********************************************************************/
  /*  Physically open the object table.                                */
  /*********************************************************************/
	if (Tdbp->OpenDB(g))
		return true;

  Tdbp->SetNext(NULL);
  Use = USE_OPEN;
	return false;
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
  return Tdbp->WriteDB(g);
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for PROXY access methods.            */
/***********************************************************************/
int TDBPRX::DeleteDB(PGLOBAL g, int irc)
  {
  return Tdbp->DeleteDB(g, irc);
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
PRXCOL::PRXCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am)
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
  Pseudo = false;
  Colnum = cdp->GetOffset();     // If columns are retrieved by number

  if (trace(1))
    htrc(" making new %sCOL C%d %s at %p\n", am, Index, Name, this);

  } // end of PRXCOL constructor

/***********************************************************************/
/*  PRXCOL constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
PRXCOL::PRXCOL(PRXCOL *col1, PTDB tdbp) : COLBLK(col1, tdbp)
  {
  Colp = col1->Colp;
  To_Val = col1->To_Val;
  Pseudo = col1->Pseudo;
  Colnum = col1->Colnum;
  } // end of PRXCOL copy constructor

/***********************************************************************/
/*  Convert an UTF-8 name to latin characters.                         */
/***********************************************************************/
char *PRXCOL::Decode(PGLOBAL g, const char *cnm)
  {
  char  *buf= (char*)PlugSubAlloc(g, NULL, strlen(cnm) + 1);
  uint   dummy_errors;
  uint32 len= copy_and_convert(buf, strlen(cnm) + 1,
                               &my_charset_latin1,
                               cnm, strlen(cnm),
                               &my_charset_utf8_general_ci,
                               &dummy_errors);
  buf[len]= '\0';
  return buf;
  } // end of Decode

/***********************************************************************/
/*  PRXCOL initialization routine.                                     */
/*  Look for the matching column in the object table.                  */
/***********************************************************************/
bool PRXCOL::Init(PGLOBAL g, PTDB tp)
  {
  if (!tp)
    tp = ((PTDBPRX)To_Tdb)->Tdbp;

  if (!(Colp = tp->ColDB(g, Name, 0)) && Colnum)
    Colp = tp->ColDB(g, NULL, Colnum);

  if (Colp) {
    MODE mode = To_Tdb->GetMode();

    // Needed for MYSQL subtables
    ((XCOLBLK*)Colp)->Name = Decode(g, Colp->GetName());

    // May not have been done elsewhere
    Colp->InitValue(g);        
    To_Val = Colp->GetValue();

    if (mode == MODE_INSERT || mode == MODE_UPDATE)
      if (Colp->SetBuffer(g, Colp->GetValue(), true, false))
        return true;

    // this may be needed by some tables (which?)
    Colp->SetColUse(ColUse);
  } else {
    sprintf(g->Message, MSG(NO_MATCHING_COL), Name, tp->GetName());
    return true;
  } // endif Colp

  return false;
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
  if (trace(2))
    htrc("PRX ReadColumn: name=%s\n", Name);

  if (Colp) {
    Colp->Eval(g);
    Value->SetValue_pval(To_Val);

    // Set null when applicable
    if (Nullable)
      Value->SetNull(Value->IsNull());

	} else {
		Value->Reset();

		// Set null when applicable
		if (Nullable)
			Value->SetNull(true);

	}	// endif Colp

  } // end of ReadColumn

/***********************************************************************/
/*  WriteColumn:                                                       */
/***********************************************************************/
void PRXCOL::WriteColumn(PGLOBAL g)
  {
  if (trace(2))
    htrc("PRX WriteColumn: name=%s\n", Name);

  if (Colp) {
    To_Val->SetValue_pval(Value);
    Colp->WriteColumn(g);
    } // endif Colp

  } // end of WriteColumn

/* ---------------------------TDBTBC class --------------------------- */

/***********************************************************************/
/*  TDBTBC class constructor.                                          */
/***********************************************************************/
TDBTBC::TDBTBC(PPRXDEF tdp) : TDBCAT(tdp)
  {
  Db  = (PSZ)tdp->Tablep->GetSchema();    
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

