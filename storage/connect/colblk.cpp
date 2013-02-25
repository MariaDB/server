/************* Colblk C++ Functions Source Code File (.CPP) ************/
/*  Name: COLBLK.CPP  Version 2.0                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1998-2013    */
/*                                                                     */
/*  This file contains the COLBLK class functions.                     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                  */
/***********************************************************************/
#include "my_global.h"

/***********************************************************************/
/*  Include required application header files                          */
/*  global.h    is header containing all global Plug declarations.     */
/*  plgdbsem.h  is header containing the DB applic. declarations.      */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "tabcol.h"
#include "colblk.h"
#include "xindex.h"
#include "xtable.h"

/***********************************************************************/
/*  COLBLK protected constructor.                                      */
/***********************************************************************/
COLBLK::COLBLK(PCOLDEF cdp, PTDB tdbp, int i)
  {
  Next = NULL;
  Index = i;
//Number = 0;
  ColUse = 0;

  if ((Cdp = cdp)) {
    Name = cdp->Name;
    Format = cdp->F;
    Opt = cdp->Opt;
    Long = cdp->Long;
    Buf_Type = cdp->Buf_Type;
    ColUse |= cdp->Flags;       // Used by CONNECT
    Nullable = !!(cdp->Flags & U_NULLS);
  } else {
    Name = NULL;
    memset(&Format, 0, sizeof(FORMAT));
    Opt = 0;
    Long = 0;
    Buf_Type = TYPE_ERROR;
    Nullable = false;
  } // endif cdp

  To_Tdb = tdbp;
  Status = BUF_NO;
//Value = NULL;                  done in XOBJECT constructor
  To_Kcol = NULL;
  } // end of COLBLK constructor

/***********************************************************************/
/*  COLBLK constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
COLBLK::COLBLK(PCOL col1, PTDB tdbp)
  {
  PCOL colp;

  // Copy the old column block to the new one
  *this = *col1;
  Next = NULL;
//To_Orig = col1;
  To_Tdb = tdbp;

#ifdef DEBTRACE
 htrc(" copying COLBLK %s from %p to %p\n", Name, col1, this);
#endif

  if (tdbp)
    // Attach the new column to the table block
    if (!tdbp->GetColumns())
      tdbp->SetColumns(this);
    else {
      for (colp = tdbp->GetColumns(); colp->Next; colp = colp->Next) ;

      colp->Next = this;
      } // endelse

  } // end of COLBLK copy constructor

/***********************************************************************/
/*  Reset the column descriptor to non evaluated yet.                  */
/***********************************************************************/
void COLBLK::Reset(void)
  {
  Status &= ~BUF_READ;
  } // end of Reset

/***********************************************************************/
/*  Compare: compares itself to an (expression) object and returns     */
/*  true if it is equivalent.                                          */
/***********************************************************************/
bool COLBLK::Compare(PXOB xp)
  {
  return (this == xp);
  } // end of Compare

/***********************************************************************/
/*  SetFormat: function used to set SELECT output format.              */
/***********************************************************************/
bool COLBLK::SetFormat(PGLOBAL g, FORMAT& fmt)
  {
  fmt = Format;

#ifdef DEBTRACE
 htrc("COLBLK: %p format=%c(%d,%d)\n", 
   this, *fmt.Type, fmt.Length, fmt.Prec);
#endif

  return false;
  } // end of SetFormat

/***********************************************************************/
/*  CheckColumn:  a column descriptor is found, say it by returning 1. */
/***********************************************************************/
int COLBLK::CheckColumn(PGLOBAL g, PSQL sqlp, PXOB &p, int &ag)
  {
  return 1;
  } // end of CheckColumn

/***********************************************************************/
/*  Eval:  get the column value from the last read record or from a    */
/*  matching Index column if there is one.                             */
/***********************************************************************/
bool COLBLK::Eval(PGLOBAL g)
  {
#ifdef DEBTRACE
 htrc("Col Eval: %s status=%.4X\n", Name, Status);
#endif

  if (!GetStatus(BUF_READ)) {
//  if (To_Tdb->IsNull())
//    Value->Reset();
    if (To_Kcol)
      To_Kcol->FillValue(Value);
    else
      ReadColumn(g);

    AddStatus(BUF_READ);
    } // endif

  return false;
  } // end of Eval

/***********************************************************************/
/*  CheckSort:                                                         */
/*  Used to check that a table is involved in the sort list items.     */
/***********************************************************************/
bool COLBLK::CheckSort(PTDB tdbp)
  {
  return (tdbp == To_Tdb);
  } // end of CheckSort

/***********************************************************************/
/*  MarkCol: see PlugMarkCol for column references to mark.            */
/***********************************************************************/
void COLBLK::MarkCol(ushort bits)
  {
  ColUse |= bits;

#ifdef DEBTRACE
 htrc(" column R%d.%s marked as %04X\n",
  To_Tdb->GetTdb_No(), Name, ColUse);
#endif
  } // end of MarkCol

/***********************************************************************/
/*  InitValue: prepare a column block for read operation.              */
/*  Now we use Format.Length for the len parameter to avoid strings    */
/*  to be truncated when converting from string to coded string.       */
/*  Added in version 1.5 is the arguments GetPrecision() and Domain    */
/*  in calling AllocateValue. Domain is used for TYPE_TOKEN only,      */
/*  but why was GetPrecision() not specified ? To be checked.          */
/***********************************************************************/
bool COLBLK::InitValue(PGLOBAL g)
  {
  if (Value)
    return false;                       // Already done

  // Allocate a Value object
  if (!(Value = AllocateValue(g, Buf_Type, Format.Length,
                                 GetPrecision(), GetDomain(),
                                 (To_Tdb) ? To_Tdb->GetCat() : NULL)))
    return true;

  Status = BUF_READY;
  Value->SetNullable(Nullable);

#ifdef DEBTRACE
 htrc(" colp=%p type=%d value=%p coluse=%.4X status=%.4X\n",
  this, Buf_Type, Value, ColUse, Status);
#endif

  return false;
  } // end of InitValue

/***********************************************************************/
/*  SetBuffer: prepare a column block for write operation.             */
/***********************************************************************/
bool COLBLK::SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check)
  {
  sprintf(g->Message, MSG(UNDEFINED_AM), "SetBuffer");
  return true;
  } // end of SetBuffer

/***********************************************************************/
/*  GetLength: returns an evaluation of the column string length.      */
/***********************************************************************/
int COLBLK::GetLengthEx(void)
  {
  return Long;
  } // end of GetLengthEx

/***********************************************************************/
/*  ReadColumn: what this routine does is to access the last line      */
/*  read from the corresponding table, extract from it the field       */
/*  corresponding to this column and convert it to buffer type.        */
/***********************************************************************/
void COLBLK::ReadColumn(PGLOBAL g)
  {
  sprintf(g->Message, MSG(UNDEFINED_AM), "ReadColumn");
  longjmp(g->jumper[g->jump_level], TYPE_COLBLK);
  } // end of ReadColumn

/***********************************************************************/
/*  WriteColumn: what this routine does is to access the last line     */
/*  read from the corresponding table, and rewrite the field           */
/*  corresponding to this column from the column buffer and type.      */
/***********************************************************************/
void COLBLK::WriteColumn(PGLOBAL g)
  {
  sprintf(g->Message, MSG(UNDEFINED_AM), "WriteColumn");
  longjmp(g->jumper[g->jump_level], TYPE_COLBLK);
  } // end of WriteColumn

/***********************************************************************/
/*  Make file output of a column descriptor block.                     */
/***********************************************************************/
void COLBLK::Print(PGLOBAL g, FILE *f, uint n)
  {
  char m[64];
  int  i;
  PCOL colp;

  memset(m, ' ', n);        // Make margin string
  m[n] = '\0';

  for (colp = To_Tdb->GetColumns(), i = 1; colp; colp = colp->Next, i++)
    if (colp == this)
      break;

  fprintf(f, "%sR%dC%d type=%d F=%.2s(%d,%d)", m, To_Tdb->GetTdb_No(),
          i, GetAmType(), Format.Type, Format.Length, Format.Prec);
  fprintf(f,
    " coluse=%04X status=%04X buftyp=%d value=%p name=%s\n",
          ColUse, Status, Buf_Type, Value, Name);
  } // end of Print

/***********************************************************************/
/*  Make string output of a column descriptor block.                   */
/***********************************************************************/
void COLBLK::Print(PGLOBAL g, char *ps, uint z)
  {
  sprintf(ps, "R%d.%s", To_Tdb->GetTdb_No(), Name);
  } // end of Print


/***********************************************************************/
/*  SPCBLK constructor.                                                */
/***********************************************************************/
SPCBLK::SPCBLK(PCOLUMN cp)
       : COLBLK((PCOLDEF)NULL, cp->GetTo_Table()->GetTo_Tdb(), 0)
  {
  Name = (char*)cp->GetName();
  Long = 0;
  Buf_Type = TYPE_ERROR;
  } // end of SPCBLK constructor

/***********************************************************************/
/*  WriteColumn: what this routine does is to access the last line     */
/*  read from the corresponding table, and rewrite the field           */
/*  corresponding to this column from the column buffer and type.      */
/***********************************************************************/
void SPCBLK::WriteColumn(PGLOBAL g)
  {
  sprintf(g->Message, MSG(SPCOL_READONLY), Name);
  longjmp(g->jumper[g->jump_level], TYPE_COLBLK);
  } // end of WriteColumn

/***********************************************************************/
/*  RIDBLK constructor for the ROWID special column.                   */
/***********************************************************************/
RIDBLK::RIDBLK(PCOLUMN cp, bool rnm) : SPCBLK(cp)
  {
  Long = 10;
  Buf_Type = TYPE_INT;
  Rnm = rnm;
  *Format.Type = 'N';
  Format.Length = 10;
  } // end of RIDBLK constructor

/***********************************************************************/
/*  ReadColumn: what this routine does is to return the ordinal        */
/*  number of the current row in the table (if Rnm is true) or in the  */
/*  current file (if Rnm is false) the same except for multiple tables.*/
/***********************************************************************/
void RIDBLK::ReadColumn(PGLOBAL g)
  {
  Value->SetValue(To_Tdb->RowNumber(g, Rnm));
  } // end of ReadColumn

/***********************************************************************/
/*  FIDBLK constructor for the FILEID special column.                  */
/***********************************************************************/
FIDBLK::FIDBLK(PCOLUMN cp) : SPCBLK(cp)
  {
//Is_Key = 2; for when the MUL table indexed reading will be implemented.
  Long = _MAX_PATH;
  Buf_Type = TYPE_STRING;
  *Format.Type = 'C';
  Format.Length = Long;
#if defined(WIN32)
  Format.Prec = 1;          // Case insensitive
#endif   // WIN32
  Constant = (!((PTDBASE)To_Tdb)->GetDef()->GetMultiple() &&
              To_Tdb->GetAmType() != TYPE_AM_PLG &&
              To_Tdb->GetAmType() != TYPE_AM_PLM);
  Fn = NULL;
  } // end of FIDBLK constructor

/***********************************************************************/
/*  ReadColumn: what this routine does is to return the current        */
/*  file ID of the table (can change for Multiple tables).             */
/***********************************************************************/
void FIDBLK::ReadColumn(PGLOBAL g)
  {
  if (Fn != ((PTDBASE)To_Tdb)->GetFile(g)) {
    char filename[_MAX_PATH];

    Fn = ((PTDBASE)To_Tdb)->GetFile(g);
    PlugSetPath(filename, Fn, ((PTDBASE)To_Tdb)->GetPath());
    Value->SetValue_psz(filename);
    } // endif Fn

  } // end of ReadColumn

/***********************************************************************/
/*  TIDBLK constructor for the TABID special column.                   */
/***********************************************************************/
TIDBLK::TIDBLK(PCOLUMN cp) : SPCBLK(cp)
  {
//Is_Key = 2; for when the MUL table indexed reading will be implemented.
  Long = 64;
  Buf_Type = TYPE_STRING;
  *Format.Type = 'C';
  Format.Length = Long;
  Format.Prec = 1;          // Case insensitive
  Constant = (To_Tdb->GetAmType() != TYPE_AM_PLG &&
              To_Tdb->GetAmType() != TYPE_AM_PLM);
  Tname = NULL;
  } // end of TIDBLK constructor

/***********************************************************************/
/*  ReadColumn: what this routine does is to return the table ID.      */
/***********************************************************************/
void TIDBLK::ReadColumn(PGLOBAL g)
  {
  if (Tname == NULL) {
    Tname = (char*)To_Tdb->GetName();
    Value->SetValue_psz(Tname);
    } // endif Tname

  } // end of ReadColumn

