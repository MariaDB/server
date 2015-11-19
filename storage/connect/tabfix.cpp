/************* TabFix C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: TABFIX                                                */
/* -------------                                                       */
/*  Version 4.9                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          1998-2015    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the TDBFIX class DB routines.                     */
/*                                                                     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant section of system dependant header files.         */
/***********************************************************************/
#include "my_global.h"
#if defined(__WIN__)
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif   // __BORLANDC__
//#include <windows.h>
#else   // !__WIN__
#if defined(UNIX)
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#else   // !UNIX
#include <io.h>
#endif  // !UNIX
#include <fcntl.h>
#endif  // !__WIN__

/***********************************************************************/
/*  Include application header files:                                  */
/***********************************************************************/
#include "global.h"      // global declares
#include "plgdbsem.h"    // DB application declares
#include "filamfix.h"
#include "filamdbf.h"
#include "tabfix.h"      // TDBFIX, FIXCOL classes declares
#include "array.h"
#include "blkfil.h"

/***********************************************************************/
/*  DB static variables.                                               */
/***********************************************************************/
extern int num_read, num_there, num_eq[2];               // Statistics
static const longlong M2G = 0x80000000;
static const longlong M4G = (longlong)2 * M2G;
char BINCOL::Endian = 'H';

/***********************************************************************/
/*  External function.                                                 */
/***********************************************************************/
USETEMP UseTemp(void);

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Implementation of the TDBFIX class.                                */
/***********************************************************************/
TDBFIX::TDBFIX(PDOSDEF tdp, PTXF txfp) : TDBDOS(tdp, txfp)
  {
  Teds = tdp->Teds;              // For BIN tables
  } // end of TDBFIX standard constructor

TDBFIX::TDBFIX(PGLOBAL g, PTDBFIX tdbp) : TDBDOS(g, tdbp)
  {
  Teds = tdbp->Teds;
  } // end of TDBFIX copy constructor

// Method
PTDB TDBFIX::CopyOne(PTABS t)
  {
  PTDB    tp;
  PGLOBAL g = t->G;

  tp = new(g) TDBFIX(g, this);

  if (Ftype < 2) {
    // File is text
    PDOSCOL cp1, cp2;

    for (cp1 = (PDOSCOL)Columns; cp1; cp1 = (PDOSCOL)cp1->GetNext()) {
      cp2 = new(g) DOSCOL(cp1, tp);  // Make a copy
      NewPointer(t, cp1, cp2);
      } // endfor cp1

  } else {
    // File is binary
    PBINCOL cp1, cp2;

    for (cp1 = (PBINCOL)Columns; cp1; cp1 = (PBINCOL)cp1->GetNext()) {
      cp2 = new(g) BINCOL(cp1, tp);  // Make a copy
      NewPointer(t, cp1, cp2);
      } // endfor cp1

  } // endif Ftype

  return tp;
  } // end of CopyOne

/***********************************************************************/
/*  Reset read/write position values.                                  */
/***********************************************************************/
void TDBFIX::ResetDB(void)
  {
  TDBDOS::ResetDB();
  } // end of ResetDB

/***********************************************************************/
/*  Allocate FIX (DOS) or BIN column description block.                */
/***********************************************************************/
PCOL TDBFIX::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  if (Ftype == RECFM_BIN)
    return new(g) BINCOL(g, cdp, this, cprec, n);
  else
    return new(g) DOSCOL(g, cdp, this, cprec, n);

  } // end of MakeCol

/***********************************************************************/
/*  Remake the indexes after the table was modified.                   */
/***********************************************************************/
int TDBFIX::ResetTableOpt(PGLOBAL g, bool dop, bool dox)
  {
  int prc, rc = RC_OK;

  To_Filter = NULL;                     // Disable filtering
//To_BlkIdx = NULL;                     // and block filtering
  To_BlkFil = NULL;                     // and index filtering
  Cardinality(g);                       // If called by create
  RestoreNrec();                        // May have been modified
  MaxSize = -1;                         // Size must be recalculated
  Cardinal = -1;                        // as well as Cardinality

  // After the table was modified the indexes
  // are invalid and we should mark them as such...
  rc = ((PDOSDEF)To_Def)->InvalidateIndex(g);

  if (dop) {
    Columns = NULL;                     // Not used anymore
    Txfp->Reset();
//  OldBlk = CurBlk = -1;
//  ReadBlks = CurNum = Rbuf = Modif = 0;
    Use = USE_READY;                    // So the table can be reopened
    Mode = MODE_ANY;                    // Just to be clean
    rc = MakeBlockValues(g);            // Redo optimization
    } // endif dop

  if (dox && (rc == RC_OK || rc == RC_INFO)) {
    // Remake eventual indexes
    Columns = NULL;                     // Not used anymore
    Txfp->Reset();                      // New start
    Use = USE_READY;                    // So the table can be reopened
    Mode = MODE_READ;                   // New mode
    prc = rc;

    if (PlgGetUser(g)->Check & CHK_OPT)
      // We must remake indexes.
      rc = MakeIndex(g, NULL, FALSE);

    rc = (rc == RC_INFO) ? prc : rc;
    } // endif dox

  return rc;
  } // end of ResetTableOpt

/***********************************************************************/
/*  Reset the Nrec and BlkSize values that can have been modified.     */
/***********************************************************************/
void TDBFIX::RestoreNrec(void)
  {
  if (!Txfp->Padded) {
    Txfp->Nrec = (To_Def && To_Def->GetElemt()) ? To_Def->GetElemt()
                                                : DOS_BUFF_LEN;
    Txfp->Blksize = Txfp->Nrec * Txfp->Lrecl;

    if (Cardinal >= 0)
      Txfp->Block = (Cardinal > 0) 
                  ? (Cardinal + Txfp->Nrec - 1) / Txfp->Nrec : 0;

    } // endif Padded

  } // end of RestoreNrec

/***********************************************************************/
/*  FIX Cardinality: returns table cardinality in number of rows.      */
/*  This function can be called with a null argument to test the       */
/*  availability of Cardinality implementation (1 yes, 0 no).          */
/***********************************************************************/
int TDBFIX::Cardinality(PGLOBAL g)
  {
  if (!g)
    return Txfp->Cardinality(g);

  if (Cardinal < 0)
    Cardinal = Txfp->Cardinality(g);

  return Cardinal;
  } // end of Cardinality

/***********************************************************************/
/*  FIX GetMaxSize: returns file size in number of lines.              */
/***********************************************************************/
int TDBFIX::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0) {
    MaxSize = Cardinality(g);

    if (MaxSize > 0 && (To_BlkFil = InitBlockFilter(g, To_Filter))
                    && !To_BlkFil->Correlated()) {
      // Use BlockTest to reduce the estimated size
      MaxSize = Txfp->MaxBlkSize(g, MaxSize);
      ResetBlockFilter(g);
      } // endif To_BlkFil

    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  FIX ResetSize: Must reset Headlen for DBF tables only.             */
/***********************************************************************/
void TDBFIX::ResetSize(void)
  {
  if (Txfp->GetAmType() == TYPE_AM_DBF)
    Txfp->Headlen = 0;

  MaxSize = Cardinal = -1;
  } // end of ResetSize

/***********************************************************************/
/*  FIX GetProgMax: get the max value for progress information.        */
/***********************************************************************/
int TDBFIX::GetProgMax(PGLOBAL g)
  {
  return Cardinality(g);
  } // end of GetProgMax

/***********************************************************************/
/*  RowNumber: return the ordinal number of the current row.           */
/***********************************************************************/
int TDBFIX::RowNumber(PGLOBAL g, bool b)
  {
  if (Txfp->GetAmType() == TYPE_AM_DBF) {
    if (!b && To_Kindex) {
      /*****************************************************************/
      /*  Don't know how to retrieve Rows from DBF file address        */
      /*  because of eventual deleted lines still in the file.         */
      /*****************************************************************/
      sprintf(g->Message, MSG(NO_ROWID_FOR_AM),
                          GetAmName(g, Txfp->GetAmType()));
      return 0;
      } // endif To_Kindex

    if (!b)
      return Txfp->GetRows();

    } // endif DBF

  return Txfp->GetRowID();
  } // end of RowNumber

/***********************************************************************/
/*  FIX tables don't use temporary files except if specified as do it. */
/***********************************************************************/
bool TDBFIX::IsUsingTemp(PGLOBAL)
  {
  // Not ready yet to handle using a temporary file with mapping
  // or while deleting from DBF files.
  return ((UseTemp() == TMP_YES && Txfp->GetAmType() != TYPE_AM_MAP &&
         !(Mode == MODE_DELETE && Txfp->GetAmType() == TYPE_AM_DBF)) ||
           UseTemp() == TMP_FORCE || UseTemp() == TMP_TEST);
  } // end of IsUsingTemp

/***********************************************************************/
/*  FIX Access Method opening routine (also used by the BIN a.m.)      */
/*  New method now that this routine is called recursively (last table */
/*  first in reverse order): index blocks are immediately linked to    */
/*  join block of next table if it exists or else are discarted.       */
/***********************************************************************/
bool TDBFIX::OpenDB(PGLOBAL g)
  {
  if (trace)
    htrc("FIX OpenDB: tdbp=%p tdb=R%d use=%d key=%p mode=%d Ftype=%d\n",
      this, Tdb_No, Use, To_Key_Col, Mode, Ftype);

  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, just replace it at its beginning.          */
    /*******************************************************************/
    if (To_Kindex)
      /*****************************************************************/
      /*  Table is to be accessed through a sorted index table.        */
      /*****************************************************************/
      To_Kindex->Reset();
    else
      Txfp->Rewind();       // see comment in Work.log

    ResetBlockFilter(g);
    return false;
    } // endif use

  if (Mode == MODE_DELETE && Txfp->GetAmType() == TYPE_AM_MAP &&
                   (!Next || UseTemp() == TMP_FORCE)) {
    // Delete all lines or using temp. Not handled in MAP mode
    Txfp = new(g) FIXFAM((PDOSDEF)To_Def);
    Txfp->SetTdbp(this);
    } // endif Mode

  /*********************************************************************/
  /*  Call Cardinality to calculate Block in the case of Func queries. */
  /*  and also in the case of multiple tables.                         */
  /*********************************************************************/
  if (Cardinality(g) < 0)
    return true;           

  /*********************************************************************/
  /*  Open according to required logical input/output mode.            */
  /*  Use conventionnal input/output functions.                        */
  /*  Treat fixed length text files as binary.                         */
  /*********************************************************************/
  if (Txfp->OpenTableFile(g))
    return true;

  Use = USE_OPEN;       // Do it now in case we are recursively called

  /*********************************************************************/
  /*  Initialize To_Line at the beginning of the block buffer.         */
  /*********************************************************************/
  To_Line = Txfp->GetBuf();                       // For WriteDB

  /*********************************************************************/
  /*  Allocate the block filter tree if evaluation is possible.        */
  /*********************************************************************/
  To_BlkFil = InitBlockFilter(g, To_Filter);

  if (trace)
    htrc("OpenFix: R%hd mode=%d BlkFil=%p\n", Tdb_No, Mode, To_BlkFil);

  /*********************************************************************/
  /*  Reset buffer access according to indexing and to mode.           */
  /*********************************************************************/
  Txfp->ResetBuffer(g);

  /*********************************************************************/
  /*  Reset statistics values.                                         */
  /*********************************************************************/
  num_read = num_there = num_eq[0] = num_eq[1] = 0;
  return false;
  } // end of OpenDB

/***********************************************************************/
/*  WriteDB: Data Base write routine for FIX access method.            */
/***********************************************************************/
int TDBFIX::WriteDB(PGLOBAL g)
  {
  return Txfp->WriteBuffer(g);
  } // end of WriteDB

// ------------------------ BINCOL functions ----------------------------

/***********************************************************************/
/*  BINCOL public constructor.                                         */
/***********************************************************************/
BINCOL::BINCOL(PGLOBAL g, PCOLDEF cdp, PTDB tp, PCOL cp, int i, PSZ am)
  : DOSCOL(g, cdp, tp, cp, i, am)
  {
  char c, *fmt = cdp->GetFmt();

  Fmt = GetDomain() ? 'C' : 'X';
  Buff = NULL;
  Eds = ((PTDBFIX)tp)->Teds;
  N = 0;
  M = GetTypeSize(Buf_Type, sizeof(longlong));
  Lim = 0;

  if (fmt) {
    for (N = 0, i = 0; fmt[i]; i++) {
      c = toupper(fmt[i]);

      if (isdigit(c))
        N = (N * 10 + (c - '0'));
      else if (c == 'L' || c == 'B' || c == 'H')
        Eds = c;
      else
        Fmt = c;

      } // endfor i

    // M is the size of the source value
    switch (Fmt) {
      case 'C': Eds = 0;              break;
      case 'X':                       break;
      case 'S': M = sizeof(short);    break;
      case 'T': M = sizeof(char);     break;
      case 'I': M = sizeof(int);      break;
      case 'G': M = sizeof(longlong); break;
      case 'R': // Real
      case 'F': M = sizeof(float);    break;
      case 'D': M = sizeof(double);   break;
      default:
        sprintf(g->Message, MSG(BAD_BIN_FMT), Fmt, Name);
        longjmp(g->jumper[g->jump_level], 11);
        } // endswitch Fmt

  } else if (IsTypeChar(Buf_Type))
    Eds = 0;

  if (Eds) {
    // This is a byte order specification
    if (!N)
      N = M;

    if (Eds != 'L' && Eds != 'B')
      Eds = Endian;

    if (N != M || Eds != Endian || IsTypeChar(Buf_Type)) {
      Buff = (char*)PlugSubAlloc(g, NULL, M);
      memset(Buff, 0, M);
      Lim = MY_MIN(N, M);
    } else
      Eds = 0;      // New format is a no op

    } // endif Eds

  } // end of BINCOL constructor

/***********************************************************************/
/*  BINCOL constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
BINCOL::BINCOL(BINCOL *col1, PTDB tdbp) : DOSCOL(col1, tdbp)
  {
  Eds = col1->Eds;
  Fmt = col1->Fmt;
  N = col1->N;
  M = col1->M;
  Lim = col1->Lim;
  } // end of BINCOL copy constructor

/***********************************************************************/
/*  Set Endian according to the host setting.                          */
/***********************************************************************/
void BINCOL::SetEndian(void)
  {
  union {
    short S;
    char  C[sizeof(short)];
    };

  S = 1;
  Endian = (C[0] == 1) ? 'L' : 'B';
  } // end of SetEndian

/***********************************************************************/
/*  ReadColumn: what this routine does is to access the last line      */
/*  read from the corresponding table and extract from it the field    */
/*  corresponding to this column.                                      */
/***********************************************************************/
void BINCOL::ReadColumn(PGLOBAL g)
  {
  char   *p = NULL;
  int     rc;
  PTDBFIX tdbp = (PTDBFIX)To_Tdb;

  if (trace > 1)
    htrc("BIN ReadColumn: col %s R%d coluse=%.4X status=%.4X buf_type=%d\n",
      Name, tdbp->GetTdb_No(), ColUse, Status, Buf_Type);

  /*********************************************************************/
  /*  If physical reading of the line was deferred, do it now.         */
  /*********************************************************************/
  if (!tdbp->IsRead())
    if ((rc = tdbp->ReadBuffer(g)) != RC_OK) {
      if (rc == RC_EF)
        sprintf(g->Message, MSG(INV_DEF_READ), rc);

      longjmp(g->jumper[g->jump_level], 11);
      } // endif

  p = tdbp->To_Line + Deplac;

  /*********************************************************************/
  /*  Set Value from the line field.                                   */
  /*********************************************************************/
  if (Eds) {
    for (int i = 0; i < Lim; i++)
      if (Eds == 'B' && Endian == 'L')
        Buff[i] = p[N - i - 1];
      else if (Eds == 'L' && Endian == 'B')
        Buff[M - i - 1] = p[i];
      else if (Endian == 'B')
        Buff[M - i - 1] = p[N - i - 1];
      else
        Buff[i] = p[i];

    p = Buff;
    } // endif Eds

  switch (Fmt) {
    case 'X':                 // Standard not converted values
      if (Eds && IsTypeChar(Buf_Type))
        Value->SetValueNonAligned<longlong>(p);
      else
        Value->SetBinValue(p);

      break;
    case 'S':                 // Short integer
      Value->SetValueNonAligned<short>(p);
      break;
    case 'T':                 // Tiny integer
      Value->SetValue(*p);
      break;
    case 'I':                 // Integer
      Value->SetValueNonAligned<int>(p);
      break;
    case 'G':                 // Large (great) integer
      Value->SetValueNonAligned<longlong>(p);
      break;
    case 'F':                 // Float
    case 'R':                 // Real
      Value->SetValueNonAligned<float>(p);
      break;
    case 'D':                 // Double
      Value->SetValueNonAligned<double>(p);
      break;
    case 'C':                 // Text
      if (Value->SetValue_char(p, Long)) {
        sprintf(g->Message, "Out of range value for column %s at row %d",
                Name, tdbp->RowNumber(g));
        PushWarning(g, tdbp);
        } // endif SetValue_char

      break;
    default:
      sprintf(g->Message, MSG(BAD_BIN_FMT), Fmt, Name);
      longjmp(g->jumper[g->jump_level], 11);
      } // endswitch Fmt

  // Set null when applicable
  if (Nullable)
    Value->SetNull(Value->IsZero());

  } // end of ReadColumn

/***********************************************************************/
/*  WriteColumn: what this routine does is to access the last line     */
/*  read from the corresponding table, and rewrite the field           */
/*  corresponding to this column from the column buffer.               */
/***********************************************************************/
void BINCOL::WriteColumn(PGLOBAL g)
  {
  char    *p, *s;
  longlong n;
  PTDBFIX  tdbp = (PTDBFIX)To_Tdb;

  if (trace) {
    htrc("BIN WriteColumn: col %s R%d coluse=%.4X status=%.4X",
          Name, tdbp->GetTdb_No(), ColUse, Status);
    htrc(" Lrecl=%d\n", tdbp->Lrecl);
    htrc("Long=%d deplac=%d coltype=%d ftype=%c\n",
          Long, Deplac, Buf_Type, *Format.Type);
    } // endif trace

  /*********************************************************************/
  /*  Check whether the new value has to be converted to Buf_Type.     */
  /*********************************************************************/
  if (Value != To_Val)
    Value->SetValue_pval(To_Val, false);    // Convert the updated value

  p = (Eds) ? Buff : tdbp->To_Line + Deplac;

  /*********************************************************************/
  /*  Check whether updating is Ok, meaning col value is not too long. */
  /*  Updating will be done only during the second pass (Status=true)  */
  /*  Conversion occurs if the external format Fmt is specified.       */
  /*********************************************************************/
  switch (Fmt) {
    case 'X':
      // Standard not converted values
      if (Eds && IsTypeChar(Buf_Type))
        *(longlong *)p = Value->GetBigintValue();
      else if (Value->GetBinValue(p, Long, Status)) {
        sprintf(g->Message, MSG(BIN_F_TOO_LONG),
                            Name, Value->GetSize(), Long);
        longjmp(g->jumper[g->jump_level], 31);
      } // endif p

      break;
    case 'S':                 // Short integer
      n = Value->GetBigintValue();

      if (n > 32767LL || n < -32768LL) {
        sprintf(g->Message, MSG(VALUE_TOO_BIG), n, Name);
        longjmp(g->jumper[g->jump_level], 31);
      } else if (Status)
        *(short *)p = (short)n;

      break;
    case 'T':                 // Tiny integer
      n = Value->GetBigintValue();

      if (n > 255LL || n < -256LL) {
        sprintf(g->Message, MSG(VALUE_TOO_BIG), n, Name);
        longjmp(g->jumper[g->jump_level], 31);
      } else if (Status)
        *p = (char)n;

      break;
    case 'I':                 // Integer
      n = Value->GetBigintValue();

      if (n > INT_MAX || n < INT_MIN) {
        sprintf(g->Message, MSG(VALUE_TOO_BIG), n, Name);
        longjmp(g->jumper[g->jump_level], 31);
      } else if (Status)
        *(int *)p = Value->GetIntValue();

      break;
    case 'G':                 // Large (great) integer
      if (Status)
        *(longlong *)p = Value->GetBigintValue();

      break;
    case 'F':                 // Float
    case 'R':                 // Real
      if (Status)
        *(float *)p = (float)Value->GetFloatValue();

      break;
    case 'D':                 // Double
      if (Status)
        *(double *)p = Value->GetFloatValue();

      break;
    case 'C':                 // Characters
      if ((n = (signed)strlen(Value->GetCharString(Buf))) > Long) {
        sprintf(g->Message, MSG(BIN_F_TOO_LONG), Name, (int) n, Long);
        longjmp(g->jumper[g->jump_level], 31);
        } // endif n

      if (Status) {
        s = Value->GetCharString(Buf);
        memset(p, ' ', Long);
        memcpy(p, s, strlen(s));
        } // endif Status

      break;
    default:
      sprintf(g->Message, MSG(BAD_BIN_FMT), Fmt, Name);
      longjmp(g->jumper[g->jump_level], 11);
    } // endswitch Fmt

  if (Eds && Status) {
    p = tdbp->To_Line + Deplac;

    for (int i = 0; i < Lim; i++)
      if (Eds == 'B' && Endian == 'L')
        p[N - i - 1] = Buff[i];
      else if (Eds == 'L' && Endian == 'B')
        p[i] = Buff[M - i - 1];
      else if (Endian == 'B')
        p[N - i - 1] = Buff[M - i - 1];
      else
        p[i] = Buff[i];

    } // endif Eds

  } // end of WriteColumn

/* ------------------------ End of TabFix ---------------------------- */
