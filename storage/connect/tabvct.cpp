/************* TabVct C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: TABVCT                                                */
/* -------------                                                       */
/*  Version 3.9                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          1999-2017    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This is the TDBVCT and VCTCOL classes implementation routines.     */
/*                                                                     */
/* WHAT YOU NEED TO COMPILE THIS PROGRAM:                              */
/* --------------------------------------                              */
/*                                                                     */
/*  REQUIRED FILES:                                                    */
/*  ---------------                                                    */
/*    TABVCT.C       - Source code                                     */
/*    PLGDBSEM.H     - DB application declaration file                 */
/*    TABDOS.H       - TABDOS classes declaration file                 */
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
#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif
//#include <windows.h>
#include <sys/stat.h>
#else
#if defined(UNIX)
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#define NO_ERROR 0
#else
#include <io.h>
#endif
#include <fcntl.h>
#endif

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  tabdos.h    is header containing the TABDOS class declarations.    */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "reldef.h"
#include "osutil.h"
#include "filamvct.h"
#include "tabdos.h"
#include "tabvct.h"
#include "valblk.h"

#if defined(UNIX)
//add dummy strerror   (NGC)
char *strerror(int num);
#endif   // UNIX

/***********************************************************************/
/*  External function.                                                 */
/***********************************************************************/
USETEMP UseTemp(void);

/***********************************************************************/
/*  Char VCT column blocks are right filled with blanks (blank = true) */
/*  Conversion of block values allowed conditionally for insert only.  */
/***********************************************************************/
PVBLK AllocValBlock(PGLOBAL, void *, int, int, int, int,
                    bool check = true, bool blank = true, bool un = false);


/* --------------------------- Class VCTDEF -------------------------- */

/***********************************************************************/
/*  DefineAM: define specific AM block values from XDB file.           */
/***********************************************************************/
bool VCTDEF::DefineAM(PGLOBAL g, LPCSTR, int poff)
  {
  DOSDEF::DefineAM(g, "BIN", poff);

  if ((Estimate = GetIntCatInfo("Estimate", 0)))
    Elemt = MY_MIN(Elemt, Estimate);

  // Split treated as INT to get default value
  Split = GetIntCatInfo("Split", (Estimate) ? 0 : 1);
  Header = GetIntCatInfo("Header", 0);

  // CONNECT must have Block/Last info for VEC tables
  if (Estimate && !Split && !Header) {
    char *fn = GetStringCatInfo(g, "Filename", "?");

    // No separate header file for urbi tables
    Header = (*fn == '?') ? 3 : 2;
    } // endif Estimate

  Recfm = RECFM_VCT;

	// poff is no more in use; This will have to be revisited
#if 0
  // For packed files the logical record length is calculated in poff
  if (poff != Lrecl) {
    Lrecl = poff;
    SetIntCatInfo("Lrecl", poff);
    } // endif poff
#endif // 0

  Padded = false;
  Blksize = 0;
  return false;
  } // end of DefineAM

#if 0
/***********************************************************************/
/*  Erase: This was made a separate routine because a strange thing    */
/*  happened when DeleteTablefile was defined for the VCTDEF class:    */
/*  when called from Catalog, the DOSDEF routine was still called even */
/*  for a VCTDEF class. It also minimizes the specific code.           */
/***********************************************************************/
bool VCTDEF::Erase(char *filename)
  {
  bool    rc = false;

  if (Split) {
    char    fpat[_MAX_PATH];
    int     i;
    PCOLDEF cdp;

    MakeFnPattern(fpat);

    for (i = 1, cdp = To_Cols; cdp; i++, cdp = cdp->GetNext()) {
      sprintf(filename, fpat, i);
//#if defined(_WIN32)
//      rc |= !DeleteFile(filename);
//#else    // UNIX
      rc |= remove(filename);
//#endif   // UNIX
      } // endfor cdp

  } else {
    rc = DOSDEF::Erase(filename);

    if (Estimate && Header == 2) {
      PlugSetPath(filename, Fn, GetPath());
      strcat(PlugRemoveType(filename, filename), ".blk");
      rc |= remove(filename);
      } // endif Header

  } // endif Split

  return rc;                                  // Return true if error
  } // end of Erase
#endif // 0

/***********************************************************************/
/*  Prepare the column file name pattern for a split table.            */
/*  This function returns the number of columns of the table.          */
/***********************************************************************/
int VCTDEF::MakeFnPattern(char *fpat)
  {
  char    pat[16];
#if defined(_WIN32)
  char    drive[_MAX_DRIVE];
#else
  char    *drive = NULL;
#endif
  char    direc[_MAX_DIR];
  char    fname[_MAX_FNAME];
  char    ftype[_MAX_EXT];          // File extention
  int     n, m, ncol = 0;
  PCOLDEF cdp;

  for (cdp = To_Cols; cdp; cdp = cdp->GetNext())
    ncol++;

  for (n = 1, m = ncol; m /= 10; n++) ;

  sprintf(pat, "%%0%dd", n);
  _splitpath(Fn, drive, direc, fname, ftype);
  strcat(fname, pat);
  _makepath(fpat, drive, direc, fname, ftype);
  PlugSetPath(fpat, fpat, GetPath());
  return ncol;
  } // end of MakeFnPattern

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB VCTDEF::GetTable(PGLOBAL g, MODE mode)
  {
  /*********************************************************************/
  /*  Allocate a TDB of the proper type.                               */
  /*  Column blocks will be allocated only when needed.                */
  /*********************************************************************/
  // Mapping not used for insert (except for true VEC not split tables)
  // or when UseTemp is forced
  bool map = Mapped && (Estimate || mode != MODE_INSERT) &&
             !(UseTemp() == TMP_FORCE &&
             (mode == MODE_UPDATE || mode == MODE_DELETE));
  PTXF txfp;
  PTDB tdbp;

  if (Multiple) {
    strcpy(g->Message, MSG(NO_MUL_VCT));
    return NULL;
    } // endif Multiple

  if (Split) {
    if (map)
      txfp = new(g) VMPFAM(this);
    else
      txfp = new(g) VECFAM(this);

  } else if (Huge)
    txfp = new(g) BGVFAM(this);
  else if (map)
    txfp = new(g) VCMFAM(this);
  else
    txfp = new(g) VCTFAM(this);

  tdbp = new(g) TDBVCT(this, txfp);

  /*********************************************************************/
  /*  For block tables, get eventually saved optimization values.      */
  /*********************************************************************/
  if (mode != MODE_INSERT)
    if (tdbp->GetBlockValues(g))
      PushWarning(g, tdbp);
//    return NULL;            // causes a crash when deleting index

  return tdbp;
  } // end of GetTable

/* --------------------------- Class TDBVCT -------------------------- */

/***********************************************************************/
/*  Implementation of the TDBVCT class.                                */
/***********************************************************************/
TDBVCT::TDBVCT(PVCTDEF tdp, PTXF txfp) : TDBFIX(tdp, txfp)
  {
  To_SetCols = NULL;
  } // end of TDBVCT standard constructor

TDBVCT::TDBVCT(PGLOBAL g, PTDBVCT tdbp) : TDBFIX(g, tdbp)
  {
  To_SetCols = tdbp->To_SetCols;
  } // end of TDBVCT copy constructor

// Method
PTDB TDBVCT::Clone(PTABS t)
  {
  PTDB    tp;
  PVCTCOL cp1, cp2;
  PGLOBAL g = t->G;        // Is this really useful ???

  tp = new(g) TDBVCT(g, this);

  for (cp1 = (PVCTCOL)Columns; cp1; cp1 = (PVCTCOL)cp1->Next) {
    cp2 = new(g) VCTCOL(cp1, tp);  // Make a copy
    NewPointer(t, cp1, cp2);
    } // endfor cp1

  return tp;
  } // end of Clone

/***********************************************************************/
/*  Allocate VCT column description block.                             */
/***********************************************************************/
PCOL TDBVCT::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  return new(g) VCTCOL(g, cdp, this, cprec, n);
  } // end of MakeCol

/***********************************************************************/
/*  VEC tables are not ready yet to use temporary files.               */
/***********************************************************************/
bool TDBVCT::IsUsingTemp(PGLOBAL)
  {
  // For developpers
  return (UseTemp() == TMP_TEST);
  } // end of IsUsingTemp

/***********************************************************************/
/*  VCT Access Method opening routine.                                 */
/*  New method now that this routine is called recursively (last table */
/*  first in reverse order): index blocks are immediately linked to    */
/*  join block of next table if it exists or else are discarted.       */
/***********************************************************************/
bool TDBVCT::OpenDB(PGLOBAL g)
  {
  if (trace(1))
    htrc("VCT OpenDB: tdbp=%p tdb=R%d use=%d key=%p mode=%d\n",
         this, Tdb_No, Use, To_Key_Col, Mode);

  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, just replace it at its beginning.          */
    /*******************************************************************/
    if (To_Kindex)
      // Table is to be accessed through a sorted index table
      To_Kindex->Reset();

    Txfp->Rewind();
    ResetBlockFilter(g);
    return false;
    } // endif Use

  /*********************************************************************/
  /*  Delete all is not handled using file mapping.                    */
  /*********************************************************************/
  if (Mode == MODE_DELETE && !Next && Txfp->GetAmType() == TYPE_AM_VMP) {
    if (IsSplit())
      Txfp = new(g) VECFAM((PVCTDEF)To_Def);
    else
      Txfp = new(g) VCTFAM((PVCTDEF)To_Def);

    Txfp->SetTdbp(this);
    } // endif Mode

  /*********************************************************************/
  /*  Open according to input/output mode required and                 */
  /*  allocate the block buffers for columns used in the query.        */
  /*********************************************************************/
  if (Txfp->OpenTableFile(g))
    return true;

  // This was not done in previous version
  Use = USE_OPEN;       // Do it now in case we are recursively called

  /*********************************************************************/
  /*  Allocate the block filter tree if evaluation is possible.        */
  /*********************************************************************/
  To_BlkFil = InitBlockFilter(g, To_Filter);

  /*********************************************************************/
  /*  Reset buffer access according to indexing and to mode.           */
  /*********************************************************************/
  Txfp->ResetBuffer(g);

  return false;
  } // end of OpenDB

/***********************************************************************/
/*  Data Base read routine for VCT access method.                      */
/*  This routine just set the new block index and record position.     */
/*  For index accessed tables the physical reading is deferred to the  */
/*  ReadColumn routine so only really used column are physically read. */
/***********************************************************************/
int TDBVCT::ReadDB(PGLOBAL g)
  {
  if (trace(1))
    htrc("VCT ReadDB: R%d Mode=%d CurBlk=%d CurNum=%d key=%p link=%p Kindex=%p\n",
         GetTdb_No(), Mode, Txfp->CurBlk, Txfp->CurNum,
         To_Key_Col, To_Link, To_Kindex);

  if (To_Kindex) {
    /*******************************************************************/
    /*  Reading is by an index table.                                  */
    /*******************************************************************/
    int recpos = To_Kindex->Fetch(g);

    switch (recpos) {
      case -1:           // End of file reached
        return RC_EF;
      case -2:           // No match for join
        return RC_NF;
      case -3:           // Same record as last non null one
//      num_there++;
        return RC_OK;
      default:
        /***************************************************************/
        /*  Set the file position according to record to read.         */
        /***************************************************************/
        if (SetRecpos(g, recpos))
          return RC_FX;

      } // endswitch recpos

    } // endif To_Kindex

  return ReadBuffer(g);
  } // end of ReadDB

/***********************************************************************/
/*  Data Base close routine for VEC access method.                     */
/***********************************************************************/
void TDBVCT::CloseDB(PGLOBAL g)
  {
  if (To_Kindex) {
    To_Kindex->Close();
    To_Kindex = NULL;
    } // endif

  Txfp->CloseTableFile(g, false);
  } // end of CloseDB

// ------------------------ VCTCOL functions ----------------------------

/***********************************************************************/
/*  VCTCOL public constructor.                                         */
/***********************************************************************/
VCTCOL::VCTCOL(PGLOBAL g, PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i)
  : DOSCOL(g, cdp, tdbp, cprec, i, "VCT")
  {
  Deplac = cdp->GetPoff();
  Clen = cdp->GetClen();       // Length of the field in the file
  ColBlk = -1;
  ColPos = -1;
  Blk = NULL;
  Modif = 0;
  } // end of VCTCOL constructor

/***********************************************************************/
/*  VCTCOL constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
VCTCOL::VCTCOL(VCTCOL *col1, PTDB tdbp) : DOSCOL(col1, tdbp)
  {
  ColBlk = col1->ColBlk;
  ColPos = col1->ColPos;
  Blk = col1->Blk;             // Should be NULL when copying ????
  Modif = col1->Modif;         // Should be 0 ????
  } // end of VCTCOL copy constructor

/***********************************************************************/
/*  SetBuffer: allocate and set the buffers needed for write operation.*/
/***********************************************************************/
bool VCTCOL::SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check)
  {
  // Eventual conversion will be done when setting ValBlk from Value.
  Value = value;        // Force To_Val == Value

  if (DOSCOL::SetBuffer(g, value, ok, check))
    return true;

  if (To_Tdb->GetMode() != MODE_INSERT) {
    // Allocate the block buffer to use for read/writing except when
    // updating a mapped VCT table and Ok is true.
    PTDBVCT tdbp = (PTDBVCT)To_Tdb;

    if (tdbp->Txfp->GetAmType() == TYPE_AM_VMP && ok) {
      Blk = AllocValBlock(g, (void*)1, Buf_Type, tdbp->Txfp->Nrec,
                          Format.Length, Format.Prec, check, true, Unsigned);
      Status |= BUF_MAPPED;  // Will point into mapped file
    } else
      Blk = AllocValBlock(g, NULL, Buf_Type, tdbp->Txfp->Nrec,
                          Format.Length, Format.Prec, check, true, Unsigned);
    } // endif Mode

  return false;
  } // end of SetBuffer

/***********************************************************************/
/*  ReadBlock: Indicate it is Ok to make updates.                      */
/***********************************************************************/
void VCTCOL::SetOk(void)
  {
  if (((PTDBVCT)To_Tdb)->Txfp->GetAmType() == TYPE_AM_VMP)
    Status |= BUF_MAPPED;

  Status |= BUF_EMPTY;
  Modif = 0;
  } // end of SetOk

/***********************************************************************/
/*  ReadBlock: Read column values from current block.                  */
/***********************************************************************/
void VCTCOL::ReadBlock(PGLOBAL g)
  {
  PVCTFAM txfp = (PVCTFAM)((PTDBVCT)To_Tdb)->Txfp;

#if defined(_DEBUG)
  if (!Blk) {
    strcpy(g->Message, MSG(TO_BLK_IS_NULL));
		throw 58;
	} // endif
#endif

  /*********************************************************************/
  /*  Read column block according to used access method.               */
  /*********************************************************************/
  if (txfp->ReadBlock(g, this))
		throw 6;

  ColBlk = txfp->CurBlk;
  ColPos = -1;                       // Any invalid position
  } // end of ReadBlock

/***********************************************************************/
/*  WriteBlock: Write back current column values for one block.        */
/*  Note: the test of Status is meant to prevent physical writing of   */
/*  the block during the checking loop in mode Update. It is set to    */
/*  BUF_EMPTY when reopening the table between the two loops.          */
/***********************************************************************/
void VCTCOL::WriteBlock(PGLOBAL g)
  {
  if (Modif && (Status & BUF_EMPTY)) {
    PVCTFAM txfp = (PVCTFAM)((PTDBVCT)To_Tdb)->Txfp;

#if defined(_DEBUG)
    if (!Blk) {
      strcpy(g->Message, MSG(BLK_IS_NULL));
			throw 56;
		} // endif
#endif

    /*******************************************************************/
    /*  Write column block according to used access method.            */
    /*******************************************************************/
    if (txfp->WriteBlock(g, this))
			throw 6;

    Modif = 0;
    } // endif Modif

  } // end of WriteBlock

/***********************************************************************/
/*  ReadColumn: what this routine does is to check whether a column    */
/*  block has been read from the file, then to extract from it the     */
/*  field corresponding to this column and convert it to buffer type.  */
/***********************************************************************/
void VCTCOL::ReadColumn(PGLOBAL g)
  {
  PTXF txfp = ((PTDBVCT)To_Tdb)->Txfp;

#if defined(_DEBUG)
  assert (!To_Kcol);
#endif

  if (trace(2))
    htrc("VCT ReadColumn: col %s R%d coluse=%.4X status=%.4X buf_type=%d\n",
         Name, To_Tdb->GetTdb_No(), ColUse, Status, Buf_Type);

  if (ColBlk != txfp->CurBlk)
    ReadBlock(g);
  else if (ColPos == txfp->CurNum)
    return;            // Value is already there

//ColBlk = txfp->CurBlk;        done in ReadBlock
  ColPos = txfp->CurNum;
  Value->SetValue_pvblk(Blk, ColPos);

  // Set null when applicable
  if (Nullable)
    Value->SetNull(Value->IsZero());

  } // end of ReadColumn

/***********************************************************************/
/*  WriteColumn: Modifications are written back into column buffer.    */
/*  On each change of block the buffer is written back to file and     */
/*  in mode Insert the buffer is filled with the block to update.      */
/***********************************************************************/
void VCTCOL::WriteColumn(PGLOBAL)
  {
  PTXF txfp = ((PTDBVCT)To_Tdb)->Txfp;;

  if (trace(2))
    htrc("VCT WriteColumn: col %s R%d coluse=%.4X status=%.4X buf_type=%d\n",
         Name, To_Tdb->GetTdb_No(), ColUse, Status, Buf_Type);

  ColBlk = txfp->CurBlk;
  ColPos = txfp->CurNum;
  Blk->SetValue(Value, ColPos);
  Modif++;
  } // end of WriteColumn

/* ------------------------ End of TabVct ---------------------------- */
