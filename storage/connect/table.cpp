/************** Table C++ Functions Source Code File (.CPP) ************/
/*  Name: TABLE.CPP  Version 2.7                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1999-2016    */
/*                                                                     */
/*  This file contains the TBX, TDB and OPJOIN classes functions.      */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                  */
/***********************************************************************/
#include "my_global.h"

/***********************************************************************/
/*  Include required application header files                          */
/*  global.h    is header containing all global Plug declarations.     */
/*  plgdbsem.h  is header containing the DB applic. declarations.      */
/*  xobject.h   is header containing XOBJECT derived classes declares. */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xtable.h"
#include "tabcol.h"
#include "filamtxt.h"
#include "tabdos.h"
//#include "catalog.h"
#include "reldef.h"

int TDB::Tnum = 0;

/***********************************************************************/
/*  Utility routines.                                                  */
/***********************************************************************/
void NewPointer(PTABS, void *, void *);
void AddPointer(PTABS, void *);

/* ---------------------------- class TDB ---------------------------- */

/***********************************************************************/
/*  TDB public constructors.                                           */
/***********************************************************************/
TDB::TDB(PTABDEF tdp) : Tdb_No(++Tnum)
  {
  Use = USE_NO;
  To_Orig = NULL;
  To_Filter = NULL;
  To_CondFil = NULL;
  Next = NULL;
  Name = (tdp) ? tdp->GetName() : NULL;
  To_Table = NULL;
  Columns = NULL;
  Degree = (tdp) ? tdp->GetDegree() : 0;
  Mode = MODE_ANY;
  Cardinal = -1;
  } // end of TDB standard constructor

TDB::TDB(PTDB tdbp) : Tdb_No(++Tnum)
  {
  Use = tdbp->Use;
  To_Orig = tdbp;
  To_Filter = NULL;
  To_CondFil = NULL;
  Next = NULL;
  Name = tdbp->Name;
  To_Table = tdbp->To_Table;
  Columns = NULL;
  Degree = tdbp->Degree;
  Mode = tdbp->Mode;
  Cardinal = tdbp->Cardinal;
  } // end of TDB copy constructor

// Methods

/***********************************************************************/
/*  RowNumber: returns the current row ordinal number.                 */
/***********************************************************************/
int TDB::RowNumber(PGLOBAL g, bool)
  {
  sprintf(g->Message, MSG(ROWID_NOT_IMPL), GetAmName(g, GetAmType()));
  return 0;
  } // end of RowNumber

PTDB TDB::Copy(PTABS t)
  {
  PTDB    tp, tdb1, tdb2 = NULL, outp = NULL;
//PGLOBAL g = t->G;        // Is this really useful ???

  for (tdb1 = this; tdb1; tdb1 = tdb1->Next) {
    tp = tdb1->CopyOne(t);

    if (!outp)
      outp = tp;
    else
      tdb2->Next = tp;

    tdb2 = tp;
    NewPointer(t, tdb1, tdb2);
    } // endfor tdb1

  return outp;
  } // end of Copy

void TDB::Print(PGLOBAL g, FILE *f, uint n)
  {
  PCOL cp;
  char m[64];

  memset(m, ' ', n);                    // Make margin string
  m[n] = '\0';

  for (PTDB tp = this; tp; tp = tp->Next) {
    fprintf(f, "%sTDB (%p) %s no=%d use=%d type=%d\n", m,
            tp, tp->Name, tp->Tdb_No, tp->Use, tp->GetAmType());

    tp->PrintAM(f, m);
    fprintf(f, "%s Columns (deg=%d):\n", m, tp->Degree);

    for (cp = tp->Columns; cp; cp = cp->GetNext())
      cp->Print(g, f, n);

    } /* endfor tp */

  } // end of Print

void TDB::Print(PGLOBAL, char *ps, uint)
  {
  sprintf(ps, "R%d.%s", Tdb_No, Name);
  } // end of Print

/* -------------------------- class TDBASE --------------------------- */

/***********************************************************************/
/*  Implementation of the TDBASE class. This is the base class to all  */
/*  classes for tables that can be joined together.                    */
/***********************************************************************/
TDBASE::TDBASE(PTABDEF tdp) : TDB(tdp)
  {
  To_Def = tdp;
  To_Link = NULL;
  To_Key_Col = NULL;
  To_Kindex = NULL;
  To_Xdp = NULL;
  To_SetCols = NULL;
  Ftype = RECFM_NAF;
  MaxSize = -1;
  Knum = 0;
  Read_Only = (tdp) ? tdp->IsReadOnly() : false;
  m_data_charset=  (tdp) ? tdp->data_charset() : NULL;
  csname = (tdp) ? tdp->csname : NULL;
  } // end of TDBASE constructor

TDBASE::TDBASE(PTDBASE tdbp) : TDB(tdbp)
  {
  To_Def = tdbp->To_Def;
  To_Link = tdbp->To_Link;
  To_Key_Col = tdbp->To_Key_Col;
  To_Kindex = tdbp->To_Kindex;
  To_Xdp = tdbp->To_Xdp;
  To_SetCols = tdbp->To_SetCols;          // ???
  Ftype = tdbp->Ftype;
  MaxSize = tdbp->MaxSize;
  Knum = tdbp->Knum;
  Read_Only = tdbp->Read_Only;
  m_data_charset= tdbp->m_data_charset;
  csname = tdbp->csname;
  } // end of TDBASE copy constructor

/***********************************************************************/
/*  Return the pointer on the DB catalog this table belongs to.        */
/***********************************************************************/
PCATLG TDBASE::GetCat(void)
  {
  return (To_Def) ? To_Def->GetCat() : NULL;
  }  // end of GetCat

/***********************************************************************/
/*  Return the pointer on the charset of this table.                   */
/***********************************************************************/
CHARSET_INFO *TDBASE::data_charset(void)
  {
  // If no DATA_CHARSET is specified, we assume that character
  // set of the remote data is the same with CHARACTER SET
  // definition of the SQL column.
  return m_data_charset ? m_data_charset : &my_charset_bin;
  } // end of data_charset

/***********************************************************************/
/*  Return the datapath of the DB this table belongs to.               */
/***********************************************************************/
PSZ TDBASE::GetPath(void)
  {
  return To_Def->GetPath();
  }  // end of GetPath

/***********************************************************************/
/*  Return true if name is a special column of this table.             */
/***********************************************************************/
bool TDBASE::IsSpecial(PSZ name)
  {
  for (PCOLDEF cdp = To_Def->GetCols(); cdp; cdp = cdp->GetNext())
    if (!stricmp(cdp->GetName(), name) && (cdp->Flags & U_SPECIAL))
      return true;   // Special column to ignore while inserting

  return false;    // Not found or not special or not inserting
  }  // end of IsSpecial

/***********************************************************************/
/*  Initialize TDBASE based column description block construction.     */
/*        name is used to call columns by name.                        */
/*        num is used by TBL to construct columns by index number.     */
/*  Note: name=Null and num=0 for constructing all columns (select *)  */
/***********************************************************************/
PCOL TDBASE::ColDB(PGLOBAL g, PSZ name, int num)
  {
  int     i;
  PCOLDEF cdp;
  PCOL    cp, colp = NULL, cprec = NULL;

  if (trace)
    htrc("ColDB: am=%d colname=%s tabname=%s num=%d\n",
          GetAmType(), SVP(name), Name, num);

  for (cdp = To_Def->GetCols(), i = 1; cdp; cdp = cdp->GetNext(), i++)
    if ((!name && !num) ||
         (name && !stricmp(cdp->GetName(), name)) || num == i) {
      /*****************************************************************/
      /*  Check for existence of desired column.                       */
      /*  Also find where to insert the new block.                     */
      /*****************************************************************/
      for (cp = Columns; cp; cp = cp->GetNext())
        if ((num && cp->GetIndex() == i) || 
            (name && !stricmp(cp->GetName(), name)))
          break;             // Found
        else if (cp->GetIndex() < i)
          cprec = cp;

      if (trace)
        htrc("cdp(%d).Name=%s cp=%p\n", i, cdp->GetName(), cp);

      /*****************************************************************/
      /*  Now take care of Column Description Block.                   */
      /*****************************************************************/
      if (cp)
        colp = cp;
      else if (!(cdp->Flags & U_SPECIAL))
        colp = MakeCol(g, cdp, cprec, i);
      else if (Mode != MODE_INSERT)
        colp = InsertSpcBlk(g, cdp);

      if (trace)
        htrc("colp=%p\n", colp);

      if (name || num)
        break;
      else if (colp && !colp->IsSpecial())
        cprec = colp;

      } // endif Name

  return (colp);
  } // end of ColDB

/***********************************************************************/
/*  InsertSpecialColumn: Put a special column ahead of the column list.*/
/***********************************************************************/
PCOL TDBASE::InsertSpecialColumn(PCOL colp)
  {
  if (!colp->IsSpecial())
    return NULL;

  colp->SetNext(Columns);
  Columns = colp;
  return colp;
  } // end of InsertSpecialColumn

/***********************************************************************/
/*  Make a special COLBLK to insert in a table.                        */
/***********************************************************************/
PCOL TDBASE::InsertSpcBlk(PGLOBAL g, PCOLDEF cdp)
  {
//char *name = cdp->GetName();
  char   *name = cdp->GetFmt();
  PCOLUMN cp;
  PCOL    colp;

  cp= new(g) COLUMN(cdp->GetName());

  if (! To_Table) {
    strcpy(g->Message, "Cannot make special column: To_Table is NULL");
    return NULL;
  } else
    cp->SetTo_Table(To_Table);

  if (!stricmp(name, "FILEID") || !stricmp(name, "FDISK") ||
      !stricmp(name, "FPATH")  || !stricmp(name, "FNAME") ||
      !stricmp(name, "FTYPE")  || !stricmp(name, "SERVID")) {
    if (!To_Def || !(To_Def->GetPseudo() & 2)) {
      sprintf(g->Message, MSG(BAD_SPEC_COLUMN));
      return NULL;
      } // endif Pseudo

    if (!stricmp(name, "FILEID"))
      colp = new(g) FIDBLK(cp, OP_XX);
    else if (!stricmp(name, "FDISK"))
      colp = new(g) FIDBLK(cp, OP_FDISK);
    else if (!stricmp(name, "FPATH"))
      colp = new(g) FIDBLK(cp, OP_FPATH);
    else if (!stricmp(name, "FNAME"))
      colp = new(g) FIDBLK(cp, OP_FNAME);
    else if (!stricmp(name, "FTYPE"))
      colp = new(g) FIDBLK(cp, OP_FTYPE);
    else
      colp = new(g) SIDBLK(cp);

  } else if (!stricmp(name, "TABID")) {
    colp = new(g) TIDBLK(cp);
  } else if (!stricmp(name, "PARTID")) {
    colp = new(g) PRTBLK(cp);
//} else if (!stricmp(name, "CONID")) {
//  colp = new(g) CIDBLK(cp);
  } else if (!stricmp(name, "ROWID")) {
    colp = new(g) RIDBLK(cp, false);
  } else if (!stricmp(name, "ROWNUM")) {
      colp = new(g) RIDBLK(cp, true);
  } else {
    sprintf(g->Message, MSG(BAD_SPECIAL_COL), name);
    return NULL;
  } // endif's name

  if (!(colp = InsertSpecialColumn(colp))) {
    sprintf(g->Message, MSG(BAD_SPECIAL_COL), name);
    return NULL;
    } // endif Insert

  return (colp);
  } // end of InsertSpcBlk

/***********************************************************************/
/*  ResetTableOpt: Wrong for this table type.                          */
/***********************************************************************/
int TDBASE::ResetTableOpt(PGLOBAL g, bool, bool)
{
  strcpy(g->Message, "This table is not indexable");
  return RC_INFO;
} // end of ResetTableOpt

/***********************************************************************/
/*  ResetKindex: set or reset the index pointer.                       */
/***********************************************************************/
void TDBASE::ResetKindex(PGLOBAL g, PKXBASE kxp)
  {
  if (To_Kindex) {
    int pos = GetRecpos();        // To be reset in Txfp

    for (PCOL colp= Columns; colp; colp= colp->GetNext())
      colp->SetKcol(NULL);

    To_Kindex->Close();           // Discard old index
    SetRecpos(g, pos);            // Ignore return value
    } // endif To_Kindex

  To_Kindex = kxp;
  } // end of ResetKindex

/***********************************************************************/
/*  SetRecpos: Replace the table at the specified position.            */
/***********************************************************************/
bool TDBASE::SetRecpos(PGLOBAL g, int)
  {
  strcpy(g->Message, MSG(SETRECPOS_NIY));
  return true;
  } // end of SetRecpos

/***********************************************************************/
/*  Methods                                                            */
/***********************************************************************/
void TDBASE::PrintAM(FILE *f, char *m)
  {
  fprintf(f, "%s AM(%d): mode=%d\n", m, GetAmType(), Mode);
  } // end of PrintAM

/***********************************************************************/
/*  Marks DOS/MAP table columns used in internal joins.                */
/*  tdb2 is the top of tree or first tdb in chained tdb's and tdbp     */
/*  points to the currently marked tdb.                                */
/*  Two questions here: exact meaning of U_J_INT ?                     */
/*  Why is the eventual reference to To_Key_Col not marked U_J_EXT ?   */
/***********************************************************************/
void TDBASE::MarkDB(PGLOBAL, PTDB tdb2)
  {
  if (trace)
    htrc("DOS MarkDB: tdbp=%p tdb2=%p\n", this, tdb2);

  } // end of MarkDB

/* ---------------------------TDBCAT class --------------------------- */

/***********************************************************************/
/*  Implementation of the TDBCAT class.                                */
/***********************************************************************/
TDBCAT::TDBCAT(PTABDEF tdp) : TDBASE(tdp)
  {
  Qrp = NULL;
  Init = false;
  N = -1;
  } // end of TDBCAT constructor

/***********************************************************************/
/*  Allocate CAT column description block.                             */
/***********************************************************************/
PCOL TDBCAT::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  PCATCOL colp;

  colp = (PCATCOL)new(g) CATCOL(cdp, this, n);

  if (cprec) {
    colp->SetNext(cprec->GetNext());
    cprec->SetNext(colp);
  } else {
    colp->SetNext(Columns);
    Columns = colp;
  } // endif cprec

  return colp;
  } // end of MakeCol

/***********************************************************************/
/*  Initialize: Get the result query block.                            */
/***********************************************************************/
bool TDBCAT::Initialize(PGLOBAL g)
  {
  if (Init)
    return false;

  if (!(Qrp = GetResult(g)))
    return true;

  if (Qrp->Truncated) {
    sprintf(g->Message, "Result limited to %d lines", Qrp->Maxres);
    PushWarning(g, this);
    } // endif Truncated

  if (Qrp->BadLines) {
    sprintf(g->Message, "%d bad lines in result", Qrp->BadLines);
    PushWarning(g, this);
    } // endif Badlines

  Init = true;
  return false;
  } // end of Initialize

/***********************************************************************/
/*  CAT: Get the number of properties.                                 */
/***********************************************************************/
int TDBCAT::GetMaxSize(PGLOBAL g __attribute__((unused)))
  {
  if (MaxSize < 0) {
//  if (Initialize(g))
//    return -1;

//  MaxSize = Qrp->Nblin;
    MaxSize = 10;             // To make MariaDB happy
    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  CAT Access Method opening routine.                                 */
/***********************************************************************/
bool TDBCAT::OpenDB(PGLOBAL g)
  {
  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open.                                            */
    /*******************************************************************/
    N = -1;
    return false;
    } // endif use

  if (Mode != MODE_READ) {
    /*******************************************************************/
    /* ODBC Info tables cannot be modified.                            */
    /*******************************************************************/
    strcpy(g->Message, "CAT tables are read only");
    return true;
    } // endif Mode

  /*********************************************************************/
  /*  Initialize the ODBC processing.                                  */
  /*********************************************************************/
  if (Initialize(g))
    return true;

  Use = USE_OPEN;
  return InitCol(g);
  } // end of OpenDB

/***********************************************************************/
/*  Initialize columns.                                                */
/***********************************************************************/
bool TDBCAT::InitCol(PGLOBAL g)
  {
  PCATCOL colp;
  PCOLRES crp;

  for (colp = (PCATCOL)Columns; colp; colp = (PCATCOL)colp->GetNext()) {
    for (crp = Qrp->Colresp; crp; crp = crp->Next)
      if ((colp->Flag && colp->Flag == crp->Fld) ||
         (!colp->Flag && !stricmp(colp->Name, crp->Name))) {
        colp->Crp = crp;
        break;
        } // endif Flag


    if (!colp->Crp /*&& !colp->GetValue()->IsConstant()*/) {
      sprintf(g->Message, "Invalid flag %d for column %s",
                          colp->Flag, colp->Name);
      return true;
		} else if (crp->Fld == FLD_SCALE || crp->Fld == FLD_RADIX)
			colp->Value->SetNullable(true);

    } // endfor colp

  return false;
  } // end of InitCol

/***********************************************************************/
/*  SetRecpos: Replace the table at the specified position.            */
/***********************************************************************/
bool TDBCAT::SetRecpos(PGLOBAL, int recpos)
  {
  N = recpos - 1;
  return false;
  } // end of SetRecpos

/***********************************************************************/
/*  Data Base read routine for CAT access method.                      */
/***********************************************************************/
int TDBCAT::ReadDB(PGLOBAL)
  {
  return (++N < Qrp->Nblin) ? RC_OK : RC_EF;
  } // end of ReadDB

/***********************************************************************/
/*  WriteDB: Data Base write routine for CAT access methods.           */
/***********************************************************************/
int TDBCAT::WriteDB(PGLOBAL g)
  {
  strcpy(g->Message, "CAT tables are read only");
  return RC_FX;
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for CAT access methods.              */
/***********************************************************************/
int TDBCAT::DeleteDB(PGLOBAL g, int)
  {
  strcpy(g->Message, "Delete not enabled for CAT tables");
  return RC_FX;
  } // end of DeleteDB

/***********************************************************************/
/*  Data Base close routine for WMI access method.                     */
/***********************************************************************/
void TDBCAT::CloseDB(PGLOBAL)
  {
  // Nothing to do
  } // end of CloseDB

// ------------------------ CATCOL functions ----------------------------

/***********************************************************************/
/*  CATCOL public constructor.                                         */
/***********************************************************************/
CATCOL::CATCOL(PCOLDEF cdp, PTDB tdbp, int n)
      : COLBLK(cdp, tdbp, n)
  {
  Tdbp = (PTDBCAT)tdbp;
  Crp = NULL;
  Flag = cdp->GetOffset();
  } // end of WMICOL constructor

/***********************************************************************/
/*  Read the next Data Source elements.                                */
/***********************************************************************/
void CATCOL::ReadColumn(PGLOBAL)
  {
	bool b = (!Crp->Kdata || Crp->Kdata->IsNull(Tdbp->N));

  // Get the value of the Name or Description property
  if (!b)
    Value->SetValue_pvblk(Crp->Kdata, Tdbp->N);
  else
    Value->Reset();

	Value->SetNull(b);
  } // end of ReadColumn

