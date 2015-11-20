/************* TabDos C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: TABDOS                                                */
/* -------------                                                       */
/*  Version 4.9                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          1998-2015    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the DOS tables classes.                           */
/*                                                                     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the System header files.              */
/***********************************************************************/
#include "my_global.h"
#if defined(__WIN__)
#include <io.h>
#include <sys\timeb.h>                   // For testing only
#include <fcntl.h>
#include <errno.h>
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif   // __BORLANDC__
//#include <windows.h>
#else   // !__WIN__
#if defined(UNIX)
#include <errno.h>
#include <unistd.h>
#else   // !UNIX
#include <io.h>
#endif  // !UNIX
#include <fcntl.h>
#endif  // !__WIN__

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  filamtxt.h  is header containing the file AM classes declarations. */
/***********************************************************************/
#include "global.h"
#include "osutil.h"
#include "plgdbsem.h"
#include "catalog.h"
#include "mycat.h"
#include "xindex.h"
#include "filamap.h"
#include "filamfix.h"
#include "filamdbf.h"
#if defined(ZIP_SUPPORT)
#include "filamzip.h"
#endif   // ZIP_SUPPORT
#include "tabdos.h"
#include "tabfix.h"
#include "tabmul.h"
#include "array.h"
#include "blkfil.h"

/***********************************************************************/
/*  DB static variables.                                               */
/***********************************************************************/
int num_read, num_there, num_eq[2];                 // Statistics

/***********************************************************************/
/*  Size of optimize file header.                                      */
/***********************************************************************/
#define NZ         4

/***********************************************************************/
/*  External function.                                                 */
/***********************************************************************/
bool    ExactInfo(void);
USETEMP UseTemp(void);

/***********************************************************************/
/*  Min and Max blocks contains zero ended fields (blank = false).     */
/*  No conversion of block values (check = true).                      */
/***********************************************************************/
PVBLK AllocValBlock(PGLOBAL, void *, int, int, int len= 0, int prec= 0,
                    bool check= true, bool blank= false, bool un= false);

/* --------------------------- Class DOSDEF -------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
DOSDEF::DOSDEF(void)
  {
  Pseudo = 3;
  Fn = NULL;
  Ofn = NULL;
  To_Indx = NULL;
  Recfm = RECFM_VAR;
  Mapped = false;
  Padded = false;
  Huge = false;
  Accept = false;
  Eof = false;
  To_Pos = NULL;
  Optimized = 0;
  AllocBlks = 0;
  Compressed = 0;
  Lrecl = 0;
  AvgLen = 0;
  Block = 0;
  Last = 0;
  Blksize = 0;
  Maxerr = 0;
  ReadMode = 0;
  Ending = 0;
  Teds = 0;
  } // end of DOSDEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values from XDB file.           */
/***********************************************************************/
bool DOSDEF::DefineAM(PGLOBAL g, LPCSTR am, int)
  {
  char   buf[8];
  bool   map = (am && (*am == 'M' || *am == 'm'));
  LPCSTR dfm = (am && (*am == 'F' || *am == 'f')) ? "F"
             : (am && (*am == 'B' || *am == 'b')) ? "B"
             : (am && !stricmp(am, "DBF"))        ? "D" : "V";

  Desc = Fn = GetStringCatInfo(g, "Filename", NULL);
  Ofn = GetStringCatInfo(g, "Optname", Fn);
  GetCharCatInfo("Recfm", (PSZ)dfm, buf, sizeof(buf));
  Recfm = (toupper(*buf) == 'F') ? RECFM_FIX :
          (toupper(*buf) == 'B') ? RECFM_BIN :
          (toupper(*buf) == 'D') ? RECFM_DBF : RECFM_VAR;
  Lrecl = GetIntCatInfo("Lrecl", 0);

  if (Recfm != RECFM_DBF)
    Compressed = GetIntCatInfo("Compressed", 0);

  Mapped = GetBoolCatInfo("Mapped", map);
//Block = GetIntCatInfo("Blocks", 0);
//Last = GetIntCatInfo("Last", 0);
  Ending = GetIntCatInfo("Ending", CRLF);

  if (Recfm == RECFM_FIX || Recfm == RECFM_BIN) {
    Huge = GetBoolCatInfo("Huge", Cat->GetDefHuge());
    Padded = GetBoolCatInfo("Padded", false);
    Blksize = GetIntCatInfo("Blksize", 0);
    Eof = (GetIntCatInfo("EOF", 0) != 0);
    Teds = toupper(*GetStringCatInfo(g, "Endian", ""));
  } else if (Recfm == RECFM_DBF) {
    Maxerr = GetIntCatInfo("Maxerr", 0);
    Accept = GetBoolCatInfo("Accept", false);
    ReadMode = GetIntCatInfo("Readmode", 0);
  } else // (Recfm == RECFM_VAR)
    AvgLen = GetIntCatInfo("Avglen", 0);

  // Ignore wrong Index definitions for catalog commands
  SetIndexInfo();
  return false;
  } // end of DefineAM

/***********************************************************************/
/*  Get the full path/name of the optization file.                     */
/***********************************************************************/
bool DOSDEF::GetOptFileName(PGLOBAL g, char *filename)
  {
  char   *ftype;

  switch (Recfm) {
    case RECFM_VAR: ftype = ".dop"; break;
    case RECFM_FIX: ftype = ".fop"; break;
    case RECFM_BIN: ftype = ".bop"; break;
    case RECFM_VCT: ftype = ".vop"; break;
    case RECFM_DBF: ftype = ".dbp"; break;
    default:
      sprintf(g->Message, MSG(INVALID_FTYPE), Recfm);
      return true;
    } // endswitch Ftype

  PlugSetPath(filename, Ofn, GetPath());
  strcat(PlugRemoveType(filename, filename), ftype);
  return false;
  } // end of GetOptFileName

/***********************************************************************/
/*  After an optimize error occured, remove all set optimize values.   */
/***********************************************************************/
void DOSDEF::RemoveOptValues(PGLOBAL g)
  {
  char    filename[_MAX_PATH];
  PCOLDEF cdp;

  // Delete settings of optimized columns
  for (cdp = To_Cols; cdp; cdp = cdp->GetNext())
    if (cdp->GetOpt()) {
      cdp->SetMin(NULL);
      cdp->SetMax(NULL);
      cdp->SetNdv(0);
      cdp->SetNbm(0);
      cdp->SetDval(NULL);
      cdp->SetBmap(NULL);
      } // endif Opt

  // Delete block position setting for not fixed tables
  To_Pos = NULL;
  AllocBlks = 0;

  // Delete any eventually ill formed non matching optimization file
  if (!GetOptFileName(g, filename))
#if defined(__WIN__)
    DeleteFile(filename);
#else    // UNIX
    remove(filename);
#endif   // __WIN__

  Optimized = 0;
  } // end of RemoveOptValues

/***********************************************************************/
/*  DeleteIndexFile: Delete DOS/UNIX index file(s) using platform API. */
/***********************************************************************/
bool DOSDEF::DeleteIndexFile(PGLOBAL g, PIXDEF pxdf)
  {
  char   *ftype;
  char    filename[_MAX_PATH];
  bool    sep, rc = false;

  if (!To_Indx)
    return false;           // No index

  // If true indexes are in separate files
  sep = GetBoolCatInfo("SepIndex", false);

  if (!sep && pxdf) {
    strcpy(g->Message, MSG(NO_RECOV_SPACE));
    return true;
    } // endif sep

  switch (Recfm) {
    case RECFM_VAR: ftype = ".dnx"; break;
    case RECFM_FIX: ftype = ".fnx"; break;
    case RECFM_BIN: ftype = ".bnx"; break;
    case RECFM_VCT: ftype = ".vnx"; break;
    case RECFM_DBF: ftype = ".dbx"; break;
    default:
      sprintf(g->Message, MSG(BAD_RECFM_VAL), Recfm);
      return true;
    } // endswitch Ftype

  /*********************************************************************/
  /*  Check for existence of an index file.                            */
  /*********************************************************************/
  if (sep) {
    // Indexes are save in separate files
#if defined(__WIN__)
    char drive[_MAX_DRIVE];
#else
    char *drive = NULL;
#endif
    char direc[_MAX_DIR];
    char fname[_MAX_FNAME];
    bool all = !pxdf;
    
    if (all)
      pxdf = To_Indx;

    for (; pxdf; pxdf = pxdf->GetNext()) {
      _splitpath(Ofn, drive, direc, fname, NULL);
      strcat(strcat(fname, "_"), pxdf->GetName());
      _makepath(filename, drive, direc, fname, ftype);
      PlugSetPath(filename, filename, GetPath());
#if defined(__WIN__)
      if (!DeleteFile(filename))
        rc |= (GetLastError() != ERROR_FILE_NOT_FOUND);
#else    // UNIX
      if (remove(filename))
        rc |= (errno != ENOENT);
#endif   // UNIX

      if (!all)
        break;

      } // endfor pxdf

  } else {  // !sep
    // Drop all indexes, delete the common file
    PlugSetPath(filename, Ofn, GetPath());
    strcat(PlugRemoveType(filename, filename), ftype);
#if defined(__WIN__)
    if (!DeleteFile(filename))
      rc = (GetLastError() != ERROR_FILE_NOT_FOUND);
#else    // UNIX
    if (remove(filename))
      rc = (errno != ENOENT);
#endif   // UNIX
  } // endif sep

  if (rc)
    sprintf(g->Message, MSG(DEL_FILE_ERR), filename);

  return rc;                        // Return true if error
  } // end of DeleteIndexFile

/***********************************************************************/
/*  InvalidateIndex: mark all indexes as invalid.                      */
/***********************************************************************/
bool DOSDEF::InvalidateIndex(PGLOBAL)
  {
  if (To_Indx)
    for (PIXDEF xp = To_Indx; xp; xp = xp->Next)
      xp->Invalid = true;

  return false;
  } // end of InvalidateIndex

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB DOSDEF::GetTable(PGLOBAL g, MODE mode)
  {
  // Mapping not used for insert
  USETEMP tmp = UseTemp();
  bool    map = Mapped && mode != MODE_INSERT &&
                !(tmp != TMP_NO && Recfm == RECFM_VAR
                                && mode == MODE_UPDATE) &&
                !(tmp == TMP_FORCE &&
                (mode == MODE_UPDATE || mode == MODE_DELETE));
  PTXF    txfp = NULL;
  PTDBASE tdbp;

  /*********************************************************************/
  /*  Allocate table and file processing class of the proper type.     */
  /*  Column blocks will be allocated only when needed.                */
  /*********************************************************************/
  if (Recfm == RECFM_DBF) {
    if (Catfunc == FNC_NO) {
      if (map)
        txfp = new(g) DBMFAM(this);
      else
        txfp = new(g) DBFFAM(this);

      tdbp = new(g) TDBFIX(this, txfp);
    } else                   // Catfunc should be 'C'
      tdbp = new(g) TDBDCL(this);

  } else if (Recfm != RECFM_VAR && Compressed < 2) {
    if (Huge)
      txfp = new(g) BGXFAM(this);
    else if (map)
      txfp = new(g) MPXFAM(this);
    else if (Compressed) {
#if defined(ZIP_SUPPORT)
      txfp = new(g) ZIXFAM(this);
#else   // !ZIP_SUPPORT
      sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "ZIP");
      return NULL;
#endif  // !ZIP_SUPPORT
    } else
      txfp = new(g) FIXFAM(this);

    tdbp = new(g) TDBFIX(this, txfp);
  } else {
    if (Compressed) {
#if defined(ZIP_SUPPORT)
      if (Compressed == 1)
        txfp = new(g) ZIPFAM(this);
      else
        txfp = new(g) ZLBFAM(this);

#else   // !ZIP_SUPPORT
      sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "ZIP");
      return NULL;
#endif  // !ZIP_SUPPORT
    } else if (map)
      txfp = new(g) MAPFAM(this);
    else
      txfp = new(g) DOSFAM(this);

    // Txfp must be set even for not multiple tables because
    // it is needed when calling Cardinality in GetBlockValues.
    tdbp = new(g) TDBDOS(this, txfp);
  } // endif Recfm

  if (Multiple)
    tdbp = new(g) TDBMUL(tdbp);
  else
    /*******************************************************************/
    /*  For block tables, get eventually saved optimization values.    */
    /*******************************************************************/
    if (tdbp->GetBlockValues(g)) {
      PushWarning(g, tdbp);
//    return NULL;            // causes a crash when deleting index
    } else if (Recfm == RECFM_VAR || Compressed > 1) {
      if (IsOptimized()) {
        if      (map) {
          txfp = new(g) MBKFAM(this);
        } else if (Compressed) {
#if defined(ZIP_SUPPORT)
          if (Compressed == 1)
            txfp = new(g) ZBKFAM(this);
          else {
            txfp->SetBlkPos(To_Pos);
            ((PZLBFAM)txfp)->SetOptimized(To_Pos != NULL);
            } // endelse
#else
          sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "ZIP");
          return NULL;
#endif
        } else
          txfp = new(g) BLKFAM(this);

        ((PTDBDOS)tdbp)->SetTxfp(txfp);
        } // endif Optimized

      } // endif Recfm

  return tdbp;
  } // end of GetTable

/* ------------------------ Class TDBDOS ----------------------------- */

/***********************************************************************/
/*  Implementation of the TDBDOS class. This is the common class that  */
/*  contain all that is common between the TDBDOS and TDBMAP classes.  */
/***********************************************************************/
TDBDOS::TDBDOS(PDOSDEF tdp, PTXF txfp) : TDBASE(tdp)
  {
  if ((Txfp = txfp))
    Txfp->SetTdbp(this);

  Lrecl = tdp->Lrecl;
  AvgLen = tdp->AvgLen;
  Ftype = tdp->Recfm;
  To_Line = NULL;
//To_BlkIdx = NULL;
  To_BlkFil = NULL;
  SavFil = NULL;
//Xeval = 0;
  Beval = 0;
  Abort = false;
  Indxd = false;
  } // end of TDBDOS standard constructor

TDBDOS::TDBDOS(PGLOBAL g, PTDBDOS tdbp) : TDBASE(tdbp)
  {
  Txfp = (g) ? tdbp->Txfp->Duplicate(g) : tdbp->Txfp;
  Lrecl = tdbp->Lrecl;
  AvgLen = tdbp->AvgLen;
  Ftype = tdbp->Ftype;
  To_Line = tdbp->To_Line;
//To_BlkIdx = tdbp->To_BlkIdx;
  To_BlkFil = tdbp->To_BlkFil;
  SavFil = tdbp->SavFil;
//Xeval = tdbp->Xeval;
  Beval = tdbp->Beval;
  Abort = tdbp->Abort;
  Indxd = tdbp->Indxd;
  } // end of TDBDOS copy constructor

// Method
PTDB TDBDOS::CopyOne(PTABS t)
  {
  PTDB    tp;
  PDOSCOL cp1, cp2;
  PGLOBAL g = t->G;

  tp = new(g) TDBDOS(g, this);

  for (cp1 = (PDOSCOL)Columns; cp1; cp1 = (PDOSCOL)cp1->GetNext()) {
    cp2 = new(g) DOSCOL(cp1, tp);  // Make a copy
    NewPointer(t, cp1, cp2);
    } // endfor cp1

  return tp;
  } // end of CopyOne

/***********************************************************************/
/*  Allocate DOS column description block.                             */
/***********************************************************************/
PCOL TDBDOS::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  return new(g) DOSCOL(g, cdp, this, cprec, n);
  } // end of MakeCol

/***********************************************************************/
/*  Print debug information.                                           */
/***********************************************************************/
void TDBDOS::PrintAM(FILE *f, char *m)
  {
  fprintf(f, "%s AM(%d): mode=%d\n", m, GetAmType(), Mode);

  if (Txfp->To_File)
    fprintf(f, "%s  File: %s\n", m, Txfp->To_File);

  } // end of PrintAM

/***********************************************************************/
/*  Remake the indexes after the table was modified.                   */
/***********************************************************************/
int TDBDOS::ResetTableOpt(PGLOBAL g, bool dop, bool dox)
  {
  int  prc = RC_OK, rc = RC_OK;

  if (!GetFileLength(g)) {
    // Void table, delete all opt and index files
    PDOSDEF defp = (PDOSDEF)To_Def;

    defp->RemoveOptValues(g);
    return (defp->DeleteIndexFile(g, NULL)) ? RC_INFO : RC_OK;
    } // endif GetFileLength

  MaxSize = -1;                        // Size must be recalculated
  Cardinal = -1;                       // as well as Cardinality

  PTXF xp = Txfp;

  To_Filter = NULL;                     // Disable filtering
//To_BlkIdx = NULL;                     // and index filtering
  To_BlkFil = NULL;                     // and block filtering

  // After the table was modified the indexes
  // are invalid and we should mark them as such...
  (void)((PDOSDEF)To_Def)->InvalidateIndex(g);

  if (dop) {
    Columns = NULL;                     // Not used anymore

    if (Txfp->Blocked) {
      // MakeBlockValues must be executed in non blocked mode
      // except for ZLIB access method.
      if        (Txfp->GetAmType() == TYPE_AM_MAP) {
        Txfp = new(g) MAPFAM((PDOSDEF)To_Def);
#if defined(ZIP_SUPPORT)
      } else if (Txfp->GetAmType() == TYPE_AM_ZIP) {
        Txfp = new(g) ZIPFAM((PDOSDEF)To_Def);
      } else if (Txfp->GetAmType() == TYPE_AM_ZLIB) {
        Txfp->Reset();
        ((PZLBFAM)Txfp)->SetOptimized(false);
#endif   // ZIP_SUPPORT
      } else if (Txfp->GetAmType() == TYPE_AM_BLK)
        Txfp = new(g) DOSFAM((PDOSDEF)To_Def);

      Txfp->SetTdbp(this);
    } else
      Txfp->Reset();

    Use = USE_READY;                    // So the table can be reopened
    Mode = MODE_ANY;                    // Just to be clean
    rc = MakeBlockValues(g);            // Redo optimization
    } // endif dop

  if (dox && (rc == RC_OK || rc == RC_INFO)) {
    // Remake eventual indexes
//  if (Mode != MODE_UPDATE)
      To_SetCols = NULL;                // Positions are changed

    Columns = NULL;                     // Not used anymore
    Txfp->Reset();                      // New start
    Use = USE_READY;                    // So the table can be reopened
    Mode = MODE_READ;                   // New mode
    prc = rc;

    if (PlgGetUser(g)->Check & CHK_OPT)
      // We must remake all indexes.
      rc = MakeIndex(g, NULL, false);

    rc = (rc == RC_INFO) ? prc : rc;
    } // endif dox

  return rc;
  } // end of ResetTableOpt

/***********************************************************************/
/*  Calculate the block sizes so block I/O can be used and also the    */
/*  Min/Max values for clustered/sorted table columns.                 */
/***********************************************************************/
int TDBDOS::MakeBlockValues(PGLOBAL g)
  {
  int        i, lg, nrec, rc, n = 0;
  int        curnum, curblk, block, savndv, savnbm;
  void      *savmin, *savmax;
  bool       blocked, xdb2 = false;
//POOLHEADER save;
  PCOLDEF    cdp;
  PDOSDEF    defp = (PDOSDEF)To_Def;
  PDOSCOL    colp = NULL;
  PDBUSER    dup = PlgGetUser(g);
  PCATLG     cat = defp->GetCat();
//void      *memp = cat->GetDescp();

  if ((nrec = defp->GetElemt()) < 2) {
    if (!To_Def->Partitioned()) {
      // This may be wrong to do in some cases
      strcpy(g->Message, MSG(TABLE_NOT_OPT));
      return RC_INFO;                   // Not to be optimized
    } else
      return RC_OK;

  } else if (GetMaxSize(g) == 0 || !(dup->Check & CHK_OPT)) {
    // Suppress the opt file firstly if the table is void,
    // secondly when it was modified with OPTIMIZATION unchecked
    // because it is no more valid.
    defp->RemoveOptValues(g);           // Erase opt file
    return RC_OK;                       // void table
  } else if (MaxSize < 0)
    return RC_FX;

  defp->SetOptimized(0);

  // Estimate the number of needed blocks
  block = (int)((MaxSize + (int)nrec - 1) / (int)nrec);

  // We have to use local variables because Txfp->CurBlk is set
  // to Rows+1 by unblocked variable length table access methods.
  curblk = -1;
  curnum = nrec - 1;
//last = 0;
  Txfp->Block = block;                  // This is useful mainly for
  Txfp->CurBlk = curblk;                // blocked tables (ZLBFAM), for
  Txfp->CurNum = curnum;                // others it is just to be clean.

  /*********************************************************************/
  /*  Allocate the array of block starting positions.                  */
  /*********************************************************************/
//if (memp)
//  save = *(PPOOLHEADER)memp;

  Txfp->BlkPos = (int*)PlugSubAlloc(g, NULL, (block + 1) * sizeof(int));

  /*********************************************************************/
  /*  Allocate the blocks for clustered columns.                       */
  /*********************************************************************/
  blocked = Txfp->Blocked;         // Save
  Txfp->Blocked = true;            // So column block can be allocated

  for (cdp = defp->GetCols(), i = 1; cdp; cdp = cdp->GetNext(), i++)
    if (cdp->GetOpt()) {
      lg = cdp->GetClen();

      if (cdp->GetFreq() && cdp->GetFreq() <= dup->Maxbmp) {
        cdp->SetXdb2(true);
        savndv = cdp->GetNdv();
        cdp->SetNdv(0);              // Reset Dval number of values
        xdb2 = true;
        savmax = cdp->GetDval();
        cdp->SetDval(PlugSubAlloc(g, NULL, cdp->GetFreq() * lg));
        savnbm = cdp->GetNbm();
        cdp->SetNbm(0);              // Prevent Bmap allocation
//      savmin = cdp->GetBmap();
//      cdp->SetBmap(PlugSubAlloc(g, NULL, block * sizeof(int)));

        if (trace)
          htrc("Dval(%p) Bmap(%p) col(%d) %s Block=%d lg=%d\n",
              cdp->GetDval(), cdp->GetBmap(), i, cdp->GetName(), block, lg);

        // colp will be initialized with proper Dval VALBLK
        colp = (PDOSCOL)MakeCol(g, cdp, colp, i);
        colp->InitValue(g);          // Allocate column value buffer
        cdp->SetNbm(savnbm);
//      cdp->SetBmap(savmin);        // Can be reused if the new size
        cdp->SetDval(savmax);        // is not greater than this one.
        cdp->SetNdv(savndv);
      } else {
        cdp->SetXdb2(false);         // Maxbmp may have been reset
        savmin = cdp->GetMin();
        savmax = cdp->GetMax();
        cdp->SetMin(PlugSubAlloc(g, NULL, block * lg));
        cdp->SetMax(PlugSubAlloc(g, NULL, block * lg));

        // Valgrind complains if there are uninitialised bytes
        // after the null character ending
        if (IsTypeChar(cdp->GetType())) {
          memset(cdp->GetMin(), 0, block * lg);
          memset(cdp->GetMax(), 0, block * lg);
          } // endif Type

        if (trace)
          htrc("min(%p) max(%p) col(%d) %s Block=%d lg=%d\n",
              cdp->GetMin(), cdp->GetMax(), i, cdp->GetName(), block, lg);

        // colp will be initialized with proper opt VALBLK's
        colp = (PDOSCOL)MakeCol(g, cdp, colp, i);
        colp->InitValue(g);          // Allocate column value buffer
        cdp->SetMin(savmin);         // Can be reused if the number
        cdp->SetMax(savmax);         // of blocks does not change.
      } // endif Freq

      } // endif Clustered

  // No optimised columns. Still useful for blocked variable tables.
  if (!colp && defp->Recfm != RECFM_VAR) {
    strcpy(g->Message, "No optimised columns");
    return RC_INFO;
    } // endif colp

  Txfp->Blocked = blocked;

  /*********************************************************************/
  /*  Now do calculate the optimization values.                        */
  /*********************************************************************/
  Mode = MODE_READ;

  if (OpenDB(g))
    return RC_FX;

  if (xdb2) {
    /*********************************************************************/
    /*  Retrieve the distinct values of XDB2 columns.                    */
    /*********************************************************************/
    if (GetDistinctColumnValues(g, nrec))
      return RC_FX;

    OpenDB(g);                   // Rewind the table file
    } // endif xdb2

#if defined(PROG_INFO)
  /*********************************************************************/
  /*  Initialize progress information                                  */
  /*********************************************************************/
  char   *p = (char *)PlugSubAlloc(g, NULL, 24 + strlen(Name));

  dup->Step = strcat(strcpy(p, MSG(OPTIMIZING)), Name);
  dup->ProgMax = GetProgMax(g);
  dup->ProgCur = 0;
#endif   // SOCKET_MODE  ||         THREAD

  /*********************************************************************/
  /*  Make block starting pos and min/max values of cluster columns.   */
  /*********************************************************************/
  while ((rc = ReadDB(g)) == RC_OK) {
    if (blocked) {
      // A blocked FAM class handles CurNum and CurBlk (ZLBFAM)
      if (!Txfp->CurNum)
        Txfp->BlkPos[Txfp->CurBlk] = Txfp->GetPos();

    } else {
      if (++curnum >= nrec) {
        if (++curblk >= block) {
          strcpy(g->Message, MSG(BAD_BLK_ESTIM));
          goto err;
        } else
          curnum = 0;

        // Get block starting position
        Txfp->BlkPos[curblk] = Txfp->GetPos();
        } // endif CurNum

//    last = curnum + 1;              // curnum is zero based
      Txfp->CurBlk = curblk;          // Used in COLDOS::SetMinMax
      Txfp->CurNum = curnum;          // Used in COLDOS::SetMinMax
    } // endif blocked

    /*******************************************************************/
    /*  Now calculate the min and max values for the cluster columns.  */
    /*******************************************************************/
    for (colp = (PDOSCOL)Columns; colp; colp = (PDOSCOL)colp->GetNext())
      if (colp->Clustered == 2) {
        if (colp->SetBitMap(g))
          goto err;

      } else
        if (colp->SetMinMax(g))
          goto err;                   // Currently: column is not sorted

#if defined(PROG_INFO)
    if (!dup->Step) {
      strcpy(g->Message, MSG(OPT_CANCELLED));
      goto err;
    } else
      dup->ProgCur = GetProgCur();
#endif   // PROG_INFO

    n++;           // Used to calculate block and last
    } // endwhile

  if (rc == RC_EF) {
    Txfp->Nrec = nrec;

#if 0 // No good because Curblk and CurNum after EOF are different
      // depending on whether the file is mapped or not mapped.
    if (blocked) {
//    Txfp->Block = Txfp->CurBlk + 1;
      Txfp->Last = (Txfp->CurNum) ? Txfp->CurNum : nrec;
//    Txfp->Last = (Txfp->CurNum) ? Txfp->CurNum + 1 : nrec;
      Txfp->Block = Txfp->CurBlk + (Txfp->Last == nrec ? 0 : 1);
    } else {
      Txfp->Block = curblk + 1;
      Txfp->Last = last;
    } // endif blocked
#endif // 0

    // New values of Block and Last
    Txfp->Block = (n + nrec - 1) / nrec;
    Txfp->Last = (n % nrec) ? (n % nrec) : nrec; 

    // This is needed to be able to calculate the last block size
    Txfp->BlkPos[Txfp->Block] = Txfp->GetNextPos();
  } else
    goto err;

  /*********************************************************************/
  /*  Save the optimization values for this table.                     */
  /*********************************************************************/
  if (!SaveBlockValues(g)) {
    defp->Block = Txfp->Block;
    defp->Last = Txfp->Last;
    CloseDB(g);
    defp->SetIntCatInfo("Blocks", Txfp->Block);
    defp->SetIntCatInfo("Last", Txfp->Last);
    return RC_OK;
    } // endif SaveBlockValues

 err:
  // Restore Desc memory suballocation
//if (memp)
//  *(PPOOLHEADER)memp = save;

  defp->RemoveOptValues(g);
  CloseDB(g);
  return RC_FX;
  } // end of MakeBlockValues

/***********************************************************************/
/*  Save the block and Min/Max values for this table.                  */
/*  The problem here is to avoid name duplication, because more than   */
/*  one data file can have the same name (but different types) and/or  */
/*  the same data file can be used with different block sizes. This is */
/*  why we use Ofn that defaults to the file name but can be set to a  */
/*  different name if necessary.                                       */
/***********************************************************************/
bool TDBDOS::SaveBlockValues(PGLOBAL g)
  {
  char    filename[_MAX_PATH];
  int     lg, n[NZ + 2];
  size_t  nbk, ndv, nbm, block = Txfp->Block;
  bool    rc = false;
  FILE   *opfile;
  PDOSCOL colp;
  PDOSDEF defp = (PDOSDEF)To_Def;

  if (defp->GetOptFileName(g, filename))
    return true;

  if (!(opfile = fopen(filename, "wb"))) {
    sprintf(g->Message, MSG(OPEN_MODE_ERROR),
            "wb", (int)errno, filename);
    strcat(strcat(g->Message, ": "), strerror(errno));

    if (trace)
      htrc("%s\n", g->Message);

    return true;
    } // endif opfile

  memset(n, 0, sizeof(n));     // To avoid valgrind warning

  if (Ftype == RECFM_VAR || defp->Compressed == 2) {
    /*******************************************************************/
    /*  Write block starting positions into the opt file.              */
    /*******************************************************************/
    block++;
    lg = sizeof(int);
    n[0] = Txfp->Last; n[1] = lg; n[2] = Txfp->Nrec; n[3] = Txfp->Block;

    if (fwrite(n, sizeof(int), NZ, opfile) != NZ) {
      sprintf(g->Message, MSG(OPT_HEAD_WR_ERR), strerror(errno));
      rc = true;
      } // endif size

    if (fwrite(Txfp->BlkPos, lg, block, opfile) != block) {
      sprintf(g->Message, MSG(OPTBLK_WR_ERR), strerror(errno));
      rc = true;
      } // endif size

    block--;                       // = Txfp->Block;
    } // endif Ftype

  /*********************************************************************/
  /*  Write the Min/Max values into the opt file.                      */
  /*********************************************************************/
  for (colp = (PDOSCOL)Columns; colp; colp = (PDOSCOL)colp->Next) {
    lg = colp->Value->GetClen();

    //  Now start the writing process
    if (colp->Clustered == 2) {
      // New XDB2 block optimization. Will be recognized when reading
      // because the column index is negated.
      ndv = colp->Ndv; nbm = colp->Nbm;
      nbk = nbm * block;
      n[0] = -colp->Index; n[1] = lg; n[2] = Txfp->Nrec; n[3] = block;
      n[4] = ndv; n[5] = nbm;

      if (fwrite(n, sizeof(int), NZ + 2, opfile) != NZ + 2) {
        sprintf(g->Message, MSG(OPT_HEAD_WR_ERR), strerror(errno));
        rc = true;
        } // endif size

      if (fwrite(colp->Dval->GetValPointer(), lg, ndv, opfile) != ndv) {
        sprintf(g->Message, MSG(OPT_DVAL_WR_ERR), strerror(errno));
        rc = true;
        } // endif size

      if (fwrite(colp->Bmap->GetValPointer(), sizeof(int), nbk, opfile) != nbk) {
        sprintf(g->Message, MSG(OPT_BMAP_WR_ERR), strerror(errno));
        rc = true;
        } // endif size

    } else {
      n[0] = colp->Index; n[1] = lg; n[2] = Txfp->Nrec; n[3] = block;

      if (fwrite(n, sizeof(int), NZ, opfile) != NZ) {
        sprintf(g->Message, MSG(OPT_HEAD_WR_ERR), strerror(errno));
        rc = true;
        } // endif size

      if (fwrite(colp->Min->GetValPointer(), lg, block, opfile) != block) {
        sprintf(g->Message, MSG(OPT_MIN_WR_ERR), strerror(errno));
        rc = true;
        } // endif size

      if (fwrite(colp->Max->GetValPointer(), lg, block, opfile) != block) {
        sprintf(g->Message, MSG(OPT_MAX_WR_ERR), strerror(errno));
        rc = true;
        } // endif size

    } // endif Clustered

    } // endfor colp

  fclose(opfile);
  return rc;
  } // end of SaveBlockValues

/***********************************************************************/
/*  Read the Min/Max values for this table.                            */
/*  The problem here is to avoid name duplication, because more than   */
/*  one data file can have the same name (but different types) and/or  */
/*  the same data file can be used with different block sizes. This is */
/*  why we use Ofn that defaults to the file name but can be set to a  */
/*  different name if necessary.                                       */
/***********************************************************************/
bool TDBDOS::GetBlockValues(PGLOBAL g)
  {
  char       filename[_MAX_PATH];
  int        i, lg, n[NZ];
  int        nrec, block = 0, last = 0, allocblk = 0;
  int        len;
  bool       newblk = false;
  size_t     ndv, nbm, nbk, blk;
  FILE      *opfile;
  PCOLDEF    cdp;
  PDOSDEF    defp = (PDOSDEF)To_Def;
  PCATLG     cat = defp->GetCat();

#if 0
  if (Mode == MODE_INSERT && Txfp->GetAmType() == TYPE_AM_DOS)
    return false;
#endif   // __WIN__

  if (defp->Optimized)
    return false;                   // Already done or to be redone

  if (Ftype == RECFM_VAR || defp->Compressed == 2) {
    /*******************************************************************/
    /*  Variable length file that can be read by block.                */
    /*******************************************************************/
    nrec = (defp->GetElemt()) ? defp->GetElemt() : 1;
      
    if (nrec > 1) {
      // The table can be declared optimized if it is void.
      // This is useful to handle Insert in optimized mode.
      char filename[_MAX_PATH];
      int  h;
      int  flen = -1;

      PlugSetPath(filename, defp->Fn, GetPath());
      h = open(filename, O_RDONLY);
      flen = (h == -1 && errno == ENOENT) ? 0 : _filelength(h);

      if (h != -1)
        close(h);

      if (!flen) {
        defp->SetOptimized(1);
        return false;
       } // endif flen

    } else
      return false;                 // Not optimisable

    cdp = defp->GetCols();
    i = 1;
  } else {
    /*******************************************************************/
    /*  Fixed length file. Opt file exists only for clustered columns. */
    /*******************************************************************/
    // Check for existence of clustered columns
    for (cdp = defp->GetCols(), i = 1; cdp; cdp = cdp->GetNext(), i++)
      if (cdp->GetOpt())
        break;

    if (!cdp)
      return false;            // No optimization needed

    if ((len = Cardinality(g)) < 0)
      return true;             // Table error
    else if (!len)
      return false;            // File does not exist yet

    block = Txfp->Block;       // Was set in Cardinality
    nrec = Txfp->Nrec;
  } // endif Ftype

  if (defp->GetOptFileName(g, filename))
    return true;

  if (!(opfile = fopen(filename, "rb")))
    return false;                   // No saved values

  if (Ftype == RECFM_VAR || defp->Compressed == 2) {
    /*******************************************************************/
    /*  Read block starting positions from the opt file.               */
    /*******************************************************************/
    lg = sizeof(int);

    if (fread(n, sizeof(int), NZ, opfile) != NZ) {
      sprintf(g->Message, MSG(OPT_HEAD_RD_ERR), strerror(errno));
      goto err;
      } // endif size

    if (n[1] != lg || n[2] != nrec) {
      sprintf(g->Message, MSG(OPT_NOT_MATCH), filename);
      goto err;
      } // endif

    last = n[0];
    block = n[3];
    blk = block + 1;

    defp->To_Pos = (int*)PlugSubAlloc(g, NULL, blk * lg);

    if (fread(defp->To_Pos, lg, blk, opfile) != blk) {
      sprintf(g->Message, MSG(OPTBLK_RD_ERR), strerror(errno));
      goto err;
      } // endif size

    } // endif Ftype

  /*********************************************************************/
  /*  Read the Min/Max values from the opt file.                       */
  /*********************************************************************/
  for (; cdp; cdp = cdp->GetNext(), i++)
    if (cdp->GetOpt()) {
      lg = cdp->GetClen();
      blk = block;

      //  Now start the reading process.
      if (fread(n, sizeof(int), NZ, opfile) != NZ) {
        sprintf(g->Message, MSG(OPT_HEAD_RD_ERR), strerror(errno));
        goto err;
        } // endif size

      if (n[0] == -i) {
        // Read the XDB2 opt values from the opt file
        if (n[1] != lg || n[2] != nrec || n[3] != block) {
          sprintf(g->Message, MSG(OPT_NOT_MATCH), filename);
          goto err;
          } // endif

        if (fread(n, sizeof(int), 2, opfile) != 2) {
          sprintf(g->Message, MSG(OPT_HEAD_RD_ERR), strerror(errno));
          goto err;
          } // endif fread

        ndv = n[0]; nbm = n[1]; nbk = nbm * blk;

        if (cdp->GetNdv() < (int)ndv || !cdp->GetDval())
          cdp->SetDval(PlugSubAlloc(g, NULL, ndv * lg));

        cdp->SetNdv((int)ndv);

        if (fread(cdp->GetDval(), lg, ndv, opfile) != ndv) {
          sprintf(g->Message, MSG(OPT_DVAL_RD_ERR), strerror(errno));
          goto err;
          } // endif size

        if (newblk || cdp->GetNbm() < (int)nbm || !cdp->GetBmap())
          cdp->SetBmap(PlugSubAlloc(g, NULL, nbk * sizeof(int)));

        cdp->SetNbm((int)nbm);

        if (fread(cdp->GetBmap(), sizeof(int), nbk, opfile) != nbk) {
          sprintf(g->Message, MSG(OPT_BMAP_RD_ERR), strerror(errno));
          goto err;
          } // endif size

        cdp->SetXdb2(true);
      } else {
        // Read the Min/Max values from the opt file
        if (n[0] != i || n[1] != lg || n[2] != nrec || n[3] != block) {
          sprintf(g->Message, MSG(OPT_NOT_MATCH), filename);
          goto err;
          } // endif

        if (newblk || !cdp->GetMin())
          cdp->SetMin(PlugSubAlloc(g, NULL, blk * lg));

        if (fread(cdp->GetMin(), lg, blk, opfile) != blk) {
          sprintf(g->Message, MSG(OPT_MIN_RD_ERR), strerror(errno));
          goto err;
          } // endif size

        if (newblk || !cdp->GetMax())
          cdp->SetMax(PlugSubAlloc(g, NULL, blk * lg));

        if (fread(cdp->GetMax(), lg, blk, opfile) != blk) {
          sprintf(g->Message, MSG(OPT_MAX_RD_ERR), strerror(errno));
          goto err;
          } // endif size

        cdp->SetXdb2(false);
      } // endif n[0] (XDB2)

      } // endif Clustered

  defp->SetBlock(block);
  defp->Last = last;     // For Cardinality
  defp->SetAllocBlks(block);
  defp->SetOptimized(1);
  fclose(opfile);
  MaxSize = -1;          // Can be refined later
  return false;

 err:
  defp->RemoveOptValues(g);
  fclose(opfile);

  // Ignore error if not in mode CHK_OPT
  return (PlgGetUser(g)->Check & CHK_OPT) != 0;
  } // end of GetBlockValues

/***********************************************************************/
/*  This fonction is used while making XDB2 block optimization.        */
/*  It constructs for each elligible columns, the sorted list of the   */
/*  distinct values existing in the column. This function uses an      */
/*  algorithm that permit to get several sets of distinct values by    */
/*  reading the table only once, which cannot be done using a standard */
/*  SQL query.                                                         */
/***********************************************************************/
bool TDBDOS::GetDistinctColumnValues(PGLOBAL g, int nrec)
  {
  char   *p;
  int     rc, blk, n = 0;
  PDOSCOL colp;
  PDBUSER dup = PlgGetUser(g);

  /*********************************************************************/
  /*  Initialize progress information                                  */
  /*********************************************************************/
  p = (char *)PlugSubAlloc(g, NULL, 48 + strlen(Name));
  dup->Step = strcat(strcpy(p, MSG(GET_DIST_VALS)), Name);
  dup->ProgMax = GetProgMax(g);
  dup->ProgCur = 0;

  while ((rc = ReadDB(g)) == RC_OK) {
    for (colp = (PDOSCOL)Columns; colp; colp = (PDOSCOL)colp->Next)
      if (colp->Clustered == 2)
        if (colp->AddDistinctValue(g))
          return true;                   // Too many distinct values

#if defined(SOCKET_MODE)
    if (SendProgress(dup)) {
      strcpy(g->Message, MSG(OPT_CANCELLED));
      return true;
    } else
#elif defined(THREAD)
    if (!dup->Step) {
      strcpy(g->Message, MSG(OPT_CANCELLED));
      return true;
    } else
#endif     // THREAD
      dup->ProgCur = GetProgCur();

    n++;
    } // endwhile

  if (rc != RC_EF)
    return true;

  // Reset the number of table blocks
//nrec = ((PDOSDEF)To_Def)->GetElemt(); (or default value)
  blk = (n + nrec - 1) / nrec;
  Txfp->Block = blk;                    // Useful mainly for ZLBFAM ???

  // Set Nbm, Bmap for XDB2 columns
  for (colp = (PDOSCOL)Columns; colp; colp = (PDOSCOL)colp->Next)
    if (colp->Clustered == 2) {
//    colp->Cdp->SetNdv(colp->Ndv);
      colp->Nbm = (colp->Ndv + MAXBMP - 1) / MAXBMP;
      colp->Bmap = AllocValBlock(g, NULL, TYPE_INT, colp->Nbm * blk);
      } // endif Clustered

  return false;
  } // end of GetDistinctColumnValues

/***********************************************************************/
/*  Analyze the filter and construct the Block Evaluation Filter.      */
/*  This is possible when a filter contains predicates implying a      */
/*  column marked as "clustered" or "sorted" matched to a constant     */
/*  argument. It is then possible by comparison against the smallest   */
/*  and largest column values in each block to determine whether the   */
/*  filter condition will be always true or always false for the block.*/
/***********************************************************************/
PBF TDBDOS::InitBlockFilter(PGLOBAL g, PFIL filp)
  {
  bool blk = Txfp->Blocked;

  if (To_BlkFil)
    return To_BlkFil;      // Already done
  else if (!filp)
    return NULL;
  else if (blk) {
    if (Txfp->GetAmType() == TYPE_AM_DBF)
      /*****************************************************************/
      /*  If RowID is used in this query, block optimization cannot be */
      /*  used because currently the file must be read sequentially.   */
      /*****************************************************************/
      for (PCOL cp = Columns; cp; cp = cp->GetNext())
        if (cp->GetAmType() == TYPE_AM_ROWID && !((RIDBLK*)cp)->GetRnm())
          return NULL;

    } // endif blk

  int  i, op = filp->GetOpc(), opm = filp->GetOpm(), n = 0;
  bool cnv[2];
  PCOL colp;
  PXOB arg[2] = {NULL,NULL};
  PBF *fp = NULL, bfp = NULL;

  switch (op) {
    case OP_EQ:
    case OP_NE:
    case OP_GT:
    case OP_GE:
    case OP_LT:
    case OP_LE:
      if (! opm) {
        for (i = 0; i < 2; i++) {
          arg[i] = filp->Arg(i);
          cnv[i] = filp->Conv(i);
          } // endfor i

        bfp = CheckBlockFilari(g, arg, op, cnv);
        break;
        } // endif !opm

      // if opm, pass thru
    case OP_IN:
      if (filp->GetArgType(0) == TYPE_COLBLK &&
          filp->GetArgType(1) == TYPE_ARRAY) {
        arg[0] = filp->Arg(0);
        arg[1] = filp->Arg(1);
        colp = (PCOL)arg[0];

        if (colp->GetTo_Tdb() == this) {
          // Block evaluation is possible for...
          if (colp->GetAmType() == TYPE_AM_ROWID) {
            // Special column ROWID and constant array, but
            // currently we don't know how to retrieve a RowID
            // from a DBF table that is not sequentially read.
//          if (Txfp->GetAmType() != TYPE_AM_DBF ||
//              ((RIDBLK*)arg[0])->GetRnm())
              bfp = new(g) BLKSPCIN(g, this, op, opm, arg, Txfp->Nrec);

          } else if (blk && Txfp->Nrec > 1 && colp->IsClustered())
            // Clustered column and constant array
            if (colp->GetClustered() == 2)
              bfp = new(g) BLKFILIN2(g, this, op, opm, arg);
            else
              bfp = new(g) BLKFILIN(g, this, op, opm, arg);

          } // endif this

#if 0
      } else if (filp->GetArgType(0) == TYPE_SCALF &&
                 filp->GetArgType(1) == TYPE_ARRAY) {
        arg[0] = filp->Arg(0);
        arg[1] = filp->Arg(1);

        if (((PSCALF)arg[0])->GetOp() == OP_ROW &&
            arg[1]->GetResultType() == TYPE_LIST) {
          PARRAY  par = (PARRAY)arg[1];
          LSTVAL *vlp = (LSTVAL*)par->GetValue();

          ((SFROW*)arg[0])->GetParms(n);

          if (n != vlp->GetN())
            return NULL;
          else
            n = par->GetNval();

          arg[1] = new(g) CONSTANT(vlp);
          fp = (PBF*)PlugSubAlloc(g, NULL, n * sizeof(PBF));
          cnv[0] = cnv[1] = false;

          if (op == OP_IN)
            op = OP_EQ;

          for (i = 0; i < n; i++) {
            par->GetNthValue(vlp, i);

            if (!(fp[i] = CheckBlockFilari(g, arg, op, cnv)))
              return NULL;

            } // endfor i

          bfp = new(g) BLKFILLOG(this, (opm == 2 ? OP_AND : OP_OR), fp, n);
          } // endif ROW
#endif // 0

      } // endif Type

      break;
    case OP_AND:
    case OP_OR:
      fp = (PBF*)PlugSubAlloc(g, NULL, 2 * sizeof(PBF));
      fp[0] = InitBlockFilter(g, (PFIL)(filp->Arg(0)));
      fp[1] = InitBlockFilter(g, (PFIL)(filp->Arg(1)));

      if (fp[0] || fp[1])
        bfp = new(g) BLKFILLOG(this, op, fp, 2);

      break;
    case OP_NOT:
      fp = (PBF*)PlugSubAlloc(g, NULL, sizeof(PBF));

      if ((*fp = InitBlockFilter(g, (PFIL)(filp->Arg(0)))))
        bfp = new(g) BLKFILLOG(this, op, fp, 1);

      break;
    case OP_LIKE:
    default:
      break;
    } // endswitch op

  return bfp;
  } // end of InitBlockFilter

/***********************************************************************/
/*  Analyze the passed arguments and construct the Block Filter.       */
/***********************************************************************/
PBF TDBDOS::CheckBlockFilari(PGLOBAL g, PXOB *arg, int op, bool *cnv)
  {
//int     i, n1, n2, ctype = TYPE_ERROR, n = 0, type[2] = {0,0};
//bool    conv = false, xdb2 = false, ok = false, b[2];
//PXOB   *xarg1, *xarg2 = NULL, xp[2];
  int     i, n = 0, type[2] = {0,0};
  bool    conv = false, xdb2 = false, ok = false;
  PXOB   *xarg2 = NULL, xp[2];
  PCOL    colp;
//LSTVAL *vlp = NULL;
//SFROW  *sfr[2];
  PBF    *fp = NULL, bfp = NULL;

  for (i = 0; i < 2; i++) {
    switch (arg[i]->GetType()) {
      case TYPE_CONST:
        type[i] = 1;
 //     ctype = arg[i]->GetResultType();
        break;
      case TYPE_COLBLK:
        conv = cnv[i];
        colp = (PCOL)arg[i];

        if (colp->GetTo_Tdb() == this) {
          if (colp->GetAmType() == TYPE_AM_ROWID) {
            // Currently we don't know how to retrieve a RowID
            // from a DBF table that is not sequentially read.
//          if (Txfp->GetAmType() != TYPE_AM_DBF ||
//              ((RIDBLK*)arg[i])->GetRnm())
              type[i] = 5;

          } else if (Txfp->Blocked && Txfp->Nrec > 1 &&
                     colp->IsClustered()) {
            type[i] = 2;
            xdb2 = colp->GetClustered() == 2;
            } // endif Clustered

        } else if (colp->GetColUse(U_CORREL)) {
          // This is a column pointing to the outer query of a
          // correlated subquery, it has a constant value during
          // each execution of the subquery.
          type[i] = 1;
//        ctype = arg[i]->GetResultType();
        } // endif this

        break;
//    case TYPE_SCALF:
//      if (((PSCALF)arg[i])->GetOp() == OP_ROW) {
//        sfr[i] = (SFROW*)arg[i];
//        type[i] = 7;
//        } // endif Op

//      break;
      default:
        break;
      } // endswitch ArgType

    if (!type[i])
      break;

    n += type[i];
    } // endfor i

  if (n == 3 || n == 6) {
    if (conv) {
      // The constant has not the good type and will not match
      // the block min/max values. Warn and abort.
      sprintf(g->Message, "Block opt: %s", MSG(VALTYPE_NOMATCH));
      PushWarning(g, this);
      return NULL;
      } // endif Conv

    if (type[0] == 1) {
      // Make it always as Column-op-Value
      *xp = arg[0];
      arg[0] = arg[1];
      arg[1] = *xp;

      switch (op) {
        case OP_GT: op = OP_LT; break;
        case OP_GE: op = OP_LE; break;
        case OP_LT: op = OP_GT; break;
        case OP_LE: op = OP_GE; break;
        } // endswitch op

      } // endif

#if defined(_DEBUG)
//  assert(arg[0]->GetResultType() == ctype);
#endif

    if (n == 3) {
      if (xdb2) {
        if (((PDOSCOL)arg[0])->GetNbm() == 1)
          bfp = new(g) BLKFILAR2(g, this, op, arg);
        else    // Multiple bitmap made of several ULONG's
          bfp = new(g) BLKFILMR2(g, this, op, arg);
      } else
        bfp = new(g) BLKFILARI(g, this, op, arg);

    } else // n = 6
      bfp = new(g) BLKSPCARI(this, op, arg, Txfp->Nrec);

#if 0
  } else if (n == 8 || n == 14) {
    if (n == 8 && ctype != TYPE_LIST) {
      // Should never happen
      strcpy(g->Message, "Block opt: bad constant");
      longjmp(g->jumper[g->jump_level], 99);
      } // endif Conv

    if (type[0] == 1) {
      // Make it always as Column-op-Value
      sfr[0] = sfr[1];
      arg[1] = arg[0];

      switch (op) {
        case OP_GT: op = OP_LT; break;
        case OP_GE: op = OP_LE; break;
        case OP_LT: op = OP_GT; break;
        case OP_LE: op = OP_GE; break;
        } // endswitch op

      } // endif

    xarg1 = sfr[0]->GetParms(n1);

    if (n == 8) {
      vlp = (LSTVAL*)arg[1]->GetValue();
      n2 = vlp->GetN();
      xp[1] = new(g) CONSTANT((PVAL)NULL);
    } else
      xarg2 = sfr[1]->GetParms(n2);

    if (n1 != n2)
      return NULL;             // Should we flag an error ?

    fp = (PBF*)PlugSubAlloc(g, NULL, n1 * sizeof(PBF));

    for (i = 0; i < n1; i++) {
      xp[0] = xarg1[i];

      if (n == 8)
        ((CONSTANT*)xp[1])->SetValue(vlp->GetSubVal(i));
      else
        xp[1] = xarg2[i];

      b[0] = b[1] = (xp[0]->GetResultType() != xp[1]->GetResultType());
      ok |= ((fp[i] = CheckBlockFilari(g, xp, op, b)) != NULL);
      } // endfor i

    if (ok)
      bfp = new(g) BLKFILLOG(this, OP_AND, fp, n1);
#endif // 0

  } // endif n

  return bfp;
  } // end of CheckBlockFilari

/***********************************************************************/
/*  ResetBlkFil: reset the block filter and restore filtering, or make */
/*  the block filter if To_Filter was not set when opening the table.  */
/***********************************************************************/
void TDBDOS::ResetBlockFilter(PGLOBAL g)
  {
  if (!To_BlkFil) {
    if (To_Filter)
      if ((To_BlkFil = InitBlockFilter(g, To_Filter))) {
        htrc("BlkFil=%p\n", To_BlkFil);
        MaxSize = -1;      // To be recalculated
        } // endif To_BlkFil
    
    return;
    } // endif To_BlkFil

  To_BlkFil->Reset(g);

  if (SavFil && !To_Filter) {
    // Restore filter if it was disabled by optimization
    To_Filter = SavFil;
    SavFil = NULL;
    } // endif

  Beval = 0;
  } // end of ResetBlockFilter

/***********************************************************************/
/*  Block optimization: evaluate the block index filter against        */
/*  the min and max values of this block and return:                   */
/*  RC_OK: if some records in the block can meet filter criteria.      */
/*  RC_NF: if no record in the block can meet filter criteria.         */
/*  RC_EF: if no record in the remaining file can meet filter criteria.*/
/*  In addition, temporarily supress filtering if all the records in   */
/*  the block meet filter criteria.                                    */
/***********************************************************************/
int TDBDOS::TestBlock(PGLOBAL g)
  {
  int rc = RC_OK;

  if (To_BlkFil && Beval != 2) {
    // Check for block filtering evaluation
    if (Beval == 1) {
      // Filter was removed for last block, restore it
      To_Filter = SavFil;
      SavFil = NULL;
      } // endif Beval

    // Check for valid records in new block
    switch (Beval = To_BlkFil->BlockEval(g)) {
      case -2:            // No more valid values in file
        rc = RC_EF;
        break;
      case -1:            // No valid values in block
        rc = RC_NF;
        break;
      case 1:             // All block values are valid
      case 2:             // All subsequent file values are Ok
        // Before suppressing the filter for the block(s) it is
        // necessary to reset the filtered columns to NOT_READ
        // so their new values are retrieved by the SELECT list.
        if (To_Filter) // Can be NULL when externally called (XDB)
          To_Filter->Reset();

        SavFil = To_Filter;
        To_Filter = NULL; // So remove filter
      } // endswitch Beval

    if (trace)
      htrc("BF Eval Beval=%d\n", Beval);

    } // endif To_BlkFil

  return rc;
  } // end of TestBlock

/***********************************************************************/
/*  Check whether we have to create/update permanent indexes.          */
/***********************************************************************/
int TDBDOS::MakeIndex(PGLOBAL g, PIXDEF pxdf, bool add)
  {
  int     k, n;
  bool    fixed, doit, sep, b = (pxdf != NULL);
  PCOL   *keycols, colp;
  PIXDEF  xdp, sxp = NULL;
  PKPDEF  kdp;
  PDOSDEF dfp;
//PCOLDEF cdp;
  PXINDEX x;
  PXLOAD  pxp;

  Mode = MODE_READ;
  Use = USE_READY;
  dfp = (PDOSDEF)To_Def;

  if (!Cardinality(g)) {
    // Void table erase eventual index file(s)
    (void)dfp->DeleteIndexFile(g, NULL);
    return RC_OK;
  } else
    fixed = Ftype != RECFM_VAR;

  // Are we are called from CreateTable or CreateIndex?
  if (pxdf) {
    if (!add && dfp->GetIndx()) {
      strcpy(g->Message, MSG(INDX_EXIST_YET));
      return RC_FX;
      } // endif To_Indx

    if (add && dfp->GetIndx()) {
      for (sxp = dfp->GetIndx(); sxp; sxp = sxp->GetNext())
        if (!stricmp(sxp->GetName(), pxdf->GetName())) {
          sprintf(g->Message, MSG(INDEX_YET_ON), pxdf->GetName(), Name);
          return RC_FX;
        } else if (!sxp->GetNext())
          break;

      sxp->SetNext(pxdf);
//    first = false;
    } else
      dfp->SetIndx(pxdf);

//  pxdf->SetDef(dfp);
  } else if (!(pxdf = dfp->GetIndx()))
    return RC_INFO;              // No index to make

  // Allocate all columns that will be used by indexes.
  // This must be done before opening the table so specific
  // column initialization can be done (in particular by TDBVCT)
  for (n = 0, xdp = pxdf; xdp; xdp = xdp->GetNext())
    for (kdp = xdp->GetToKeyParts(); kdp; kdp = kdp->GetNext()) {
      if (!(colp = ColDB(g, kdp->GetName(), 0))) {
        sprintf(g->Message, MSG(INDX_COL_NOTIN), kdp->GetName(), Name);
        goto err;
      } else if (colp->GetResultType() == TYPE_DECIM) {
        sprintf(g->Message, "Decimal columns are not indexable yet");
        goto err;
      } // endif Type

      colp->InitValue(g);
      n = MY_MAX(n, xdp->GetNparts());
      } // endfor kdp

  keycols = (PCOL*)PlugSubAlloc(g, NULL, n * sizeof(PCOL));
  sep = dfp->GetBoolCatInfo("SepIndex", false);

  /*********************************************************************/
  /*  Construct and save the defined indexes.                          */
  /*********************************************************************/
  for (xdp = pxdf; xdp; xdp = xdp->GetNext())
    if (!OpenDB(g)) {
      if (xdp->IsAuto() && fixed)
        // Auto increment key and fixed file: use an XXROW index
        continue;      // XXROW index doesn't need to be made

      // On Update, redo only indexes that are modified
      doit = !To_SetCols;
      n = 0;

      if (sxp)
        xdp->SetID(sxp->GetID() + 1);

      for (kdp = xdp->GetToKeyParts(); kdp; kdp = kdp->GetNext()) {
        // Check whether this column was updated
        for (colp = To_SetCols; !doit && colp; colp = colp->GetNext())
          if (!stricmp(kdp->GetName(), colp->GetName()))
            doit = true;

        keycols[n++] = ColDB(g, kdp->GetName(), 0);
        } // endfor kdp

      // If no indexed columns were updated, don't remake the index
      // if indexes are in separate files.
      if (!doit && sep)
        continue;

      k = xdp->GetNparts();

      // Make the index and save it
      if (dfp->Huge)
        pxp = new(g) XHUGE;
      else
        pxp = new(g) XFILE;

      if (k == 1)            // Simple index
        x = new(g) XINDXS(this, xdp, pxp, keycols);
      else                   // Multi-Column index
        x = new(g) XINDEX(this, xdp, pxp, keycols);

      if (!x->Make(g, sxp)) {
        // Retreive define values from the index
        xdp->SetMaxSame(x->GetMaxSame());
//      xdp->SetSize(x->GetSize());

        // store KXYCOL Mxs in KPARTDEF Mxsame
        xdp->SetMxsame(x);

#if defined(TRACE)
        printf("Make done...\n");
#endif   // TRACE

//      if (x->GetSize() > 0)
          sxp = xdp;

        xdp->SetInvalid(false);
      } else
        goto err;

    } else
      return RC_INFO;     // Error or Physical table does not exist

  if (Use == USE_OPEN)
    CloseDB(g);

  return RC_OK;

err:
  if (sxp)
    sxp->SetNext(NULL);
  else
    dfp->SetIndx(NULL);

  return RC_FX;
  } // end of MakeIndex

/***********************************************************************/
/*  Make a dynamic index.                                              */
/***********************************************************************/
bool TDBDOS::InitialyzeIndex(PGLOBAL g, volatile PIXDEF xdp, bool sorted)
  {
  int     k, rc;
  volatile bool dynamic;
  bool    brc;
  PCOL    colp;
  PCOLDEF cdp;
  PVAL    valp;
  PXLOAD  pxp;
  volatile PKXBASE kxp;
  PKPDEF  kdp;

  if (!xdp && !(xdp = To_Xdp)) {
    strcpy(g->Message, "NULL dynamic index");
    return true;
  } else
    dynamic = To_Filter && xdp->IsUnique() && xdp->IsDynamic();
//  dynamic = To_Filter && xdp->IsDynamic();      NIY

  // Allocate the key columns definition block
  Knum = xdp->GetNparts();
  To_Key_Col = (PCOL*)PlugSubAlloc(g, NULL, Knum * sizeof(PCOL));

  // Get the key column description list
  for (k = 0, kdp = xdp->GetToKeyParts(); kdp; kdp = kdp->GetNext())
    if (!(colp = ColDB(g, kdp->GetName(), 0)) || colp->InitValue(g)) {
      sprintf(g->Message, "Wrong column %s", kdp->GetName());
      return true;
    } else
      To_Key_Col[k++] = colp;

#if defined(_DEBUG)
  if (k != Knum) {
    sprintf(g->Message, "Key part number mismatch for %s",
                        xdp->GetName());
    return 0;
    } // endif k
#endif   // _DEBUG

  // Allocate the pseudo constants that will contain the key values
  To_Link = (PXOB*)PlugSubAlloc(g, NULL, Knum * sizeof(PXOB));

  for (k = 0, kdp = xdp->GetToKeyParts(); kdp; k++, kdp = kdp->GetNext()) {
    if ((cdp = Key(k)->GetCdp()))
      valp = AllocateValue(g, cdp->GetType(), cdp->GetLength());
    else {                        // Special column ?
      colp = Key(k);
      valp = AllocateValue(g, colp->GetResultType(), colp->GetLength());
    } // endif cdp

    To_Link[k]= new(g) CONSTANT(valp);
    } // endfor k

  // Make the index on xdp
  if (!xdp->IsAuto()) {
    if (!dynamic) {
      if (((PDOSDEF)To_Def)->Huge)
        pxp = new(g) XHUGE;
      else
        pxp = new(g) XFILE;

    } else
      pxp = NULL;

    if (Knum == 1)            // Single index
      kxp = new(g) XINDXS(this, xdp, pxp, To_Key_Col, To_Link);
    else                      // Multi-Column index
      kxp = new(g) XINDEX(this, xdp, pxp, To_Key_Col, To_Link);

  } else                      // Column contains same values as ROWID
    kxp = new(g) XXROW(this);

  //  Prepare error return
  if (g->jump_level == MAX_JUMP) {
    strcpy(g->Message, MSG(TOO_MANY_JUMPS));
    return true;
    } // endif

  if (!(rc = setjmp(g->jumper[++g->jump_level])) != 0) {
    if (dynamic) {
      ResetBlockFilter(g);
      kxp->SetDynamic(dynamic);
      brc = kxp->Make(g, xdp);
    } else
      brc = kxp->Init(g);

    if (!brc) {
      if (Txfp->GetAmType() == TYPE_AM_BLK) {
        // Cannot use indexing in DOS block mode
        Txfp = new(g) DOSFAM((PBLKFAM)Txfp, (PDOSDEF)To_Def);
        Txfp->AllocateBuffer(g);
        To_BlkFil = NULL;
        } // endif AmType

      To_Kindex= kxp;

      if (!(sorted && To_Kindex->IsSorted()) &&
          ((Mode == MODE_UPDATE && IsUsingTemp(g)) ||
           (Mode == MODE_DELETE && Txfp->GetAmType() != TYPE_AM_DBF)))
        Indxd = true;

      } // endif brc

  } else
    brc = true;

  g->jump_level--;
  return brc;
  } // end of InitialyzeIndex

/***********************************************************************/
/*  DOS GetProgMax: get the max value for progress information.        */
/***********************************************************************/
int TDBDOS::GetProgMax(PGLOBAL g)
  {
  return (To_Kindex) ? GetMaxSize(g) : GetFileLength(g);
  } // end of GetProgMax

/***********************************************************************/
/*  DOS GetProgCur: get the current value for progress information.    */
/***********************************************************************/
int TDBDOS::GetProgCur(void)
  {
  return (To_Kindex) ? To_Kindex->GetCur_K() + 1 : GetRecpos();
  } // end of GetProgCur

/***********************************************************************/
/*  RowNumber: return the ordinal number of the current row.           */
/***********************************************************************/
int TDBDOS::RowNumber(PGLOBAL g, bool)
  {
  if (To_Kindex) {
    /*******************************************************************/
    /*  Don't know how to retrieve RowID from file address.            */
    /*******************************************************************/
    sprintf(g->Message, MSG(NO_ROWID_FOR_AM),
                        GetAmName(g, Txfp->GetAmType()));
    return 0;
  } else
    return Txfp->GetRowID();

  } // end of RowNumber

/***********************************************************************/
/*  DOS Cardinality: returns table cardinality in number of rows.      */
/*  This function can be called with a null argument to test the       */
/*  availability of Cardinality implementation (1 yes, 0 no).          */
/***********************************************************************/
int TDBDOS::Cardinality(PGLOBAL g)
  {
  int n = Txfp->Cardinality(NULL);

  if (!g)
    return (Mode == MODE_ANY) ? 1 : n;

  if (Cardinal < 0) {
    if (!Txfp->Blocked && n == 0) {
      // Info command, we try to return exact row number
      PDOSDEF dfp = (PDOSDEF)To_Def;
      PIXDEF  xdp = dfp->To_Indx;

      if (xdp && xdp->IsValid()) {
        // Cardinality can be retreived from one index
        PXLOAD  pxp;
    
        if (dfp->Huge)
          pxp = new(g) XHUGE;
        else
          pxp = new(g) XFILE;
    
        PXINDEX kxp = new(g) XINDEX(this, xdp, pxp, NULL, NULL);
    
        if (!(kxp->GetAllSizes(g, Cardinal)))
          return Cardinal;
    
        } // endif Mode

      if (Mode == MODE_ANY && ExactInfo()) {
        // Using index impossible or failed, do it the hard way
        Mode = MODE_READ;
        To_Line = (char*)PlugSubAlloc(g, NULL, Lrecl + 1);
    
        if (Txfp->OpenTableFile(g))
          return (Cardinal = Txfp->Cardinality(g));
    
        for (Cardinal = 0; n != RC_EF;)
          if (!(n = Txfp->ReadBuffer(g)))
            Cardinal++;
    
        Txfp->CloseTableFile(g, false);
        Mode = MODE_ANY;
      } else {
        // Return the best estimate
        int len = GetFileLength(g);

        if (len >= 0) {
          int rec;

          if (trace)
            htrc("Estimating lines len=%d ending=%d/n",
                  len, ((PDOSDEF)To_Def)->Ending);

          /*************************************************************/
          /* Estimate the number of lines in the table (if not known)  */
          /* by dividing the file length by the average record length. */
          /*************************************************************/
          rec = ((PDOSDEF)To_Def)->Ending;

          if (AvgLen <= 0)          // No given average estimate
            rec += EstimatedLength();
          else   // An estimate was given for the average record length
            rec += AvgLen;

          Cardinal = (len + rec - 1) / rec;

          if (trace)
            htrc("avglen=%d MaxSize%d\n", rec, Cardinal);

          } // endif len

      } // endif Mode

    } else
      Cardinal = Txfp->Cardinality(g);

    } // endif Cardinal

  return Cardinal;
  } // end of Cardinality

/***********************************************************************/
/*  DOS GetMaxSize: returns file size estimate in number of lines.     */
/*  This function covers variable record length files.                 */
/***********************************************************************/
int TDBDOS::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize >= 0)
    return MaxSize;

  if (!Cardinality(NULL)) {
    int len = GetFileLength(g);

    if (len >= 0) {
      int rec;

      if (trace)
        htrc("Estimating lines len=%d ending=%d/n",
              len, ((PDOSDEF)To_Def)->Ending);

      /*****************************************************************/
      /*  Estimate the number of lines in the table (if not known) by  */
      /*  dividing the file length by minimum record length.           */
      /*****************************************************************/
      rec = EstimatedLength() + ((PDOSDEF)To_Def)->Ending;
      MaxSize = (len + rec - 1) / rec;

      if (trace)
        htrc("avglen=%d MaxSize%d\n", rec, MaxSize);

      } // endif len

  } else
    MaxSize = Cardinality(g);

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  DOS EstimatedLength. Returns an estimated minimum line length.     */
/***********************************************************************/
int TDBDOS::EstimatedLength(void)
  {
  int     dep = 0;
  PCOLDEF cdp = To_Def->GetCols();

  if (!cdp->GetNext()) {
    // One column table, we are going to return a ridiculous
    // result if we set dep to 1
    dep = 1 + cdp->GetLong() / 20;           // Why 20 ?????
  } else for (; cdp; cdp = cdp->GetNext())
		if (!(cdp->Flags & (U_VIRTUAL|U_SPECIAL)))
      dep = MY_MAX(dep, cdp->GetOffset());

  return (int)dep;
  } // end of Estimated Length

/***********************************************************************/
/*  DOS tables favor the use temporary files for Update.               */
/***********************************************************************/
bool TDBDOS::IsUsingTemp(PGLOBAL)
  {
  USETEMP utp = UseTemp();

  return (utp == TMP_YES || utp == TMP_FORCE ||
         (utp == TMP_AUTO && Mode == MODE_UPDATE));
  } // end of IsUsingTemp

/***********************************************************************/
/*  DOS Access Method opening routine.                                 */
/*  New method now that this routine is called recursively (last table */
/*  first in reverse order): index blocks are immediately linked to    */
/*  join block of next table if it exists or else are discarted.       */
/***********************************************************************/
bool TDBDOS::OpenDB(PGLOBAL g)
  {
  if (trace)
    htrc("DOS OpenDB: tdbp=%p tdb=R%d use=%d mode=%d\n",
          this, Tdb_No, Use, Mode);

  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, just replace it at its beginning.          */
    /*******************************************************************/
    if (!To_Kindex) {
      Txfp->Rewind();       // see comment in Work.log

      if (SkipHeader(g))
        return true;

    } else
      /*****************************************************************/
      /*  Table is to be accessed through a sorted index table.        */
      /*****************************************************************/
      To_Kindex->Reset();

    ResetBlockFilter(g);
    return false;
    } // endif use

  if (Mode == MODE_DELETE && !Next && Txfp->GetAmType() != TYPE_AM_DOS) {
    // Delete all lines. Not handled in MAP or block mode
    Txfp = new(g) DOSFAM((PDOSDEF)To_Def);
    Txfp->SetTdbp(this);
  } else if (Txfp->Blocked && (Mode == MODE_DELETE ||
             (Mode == MODE_UPDATE && UseTemp() != TMP_NO))) {
    /*******************************************************************/
    /*  Delete is not currently handled in block mode neither Update   */
    /*  when using a temporary file.                                   */
    /*******************************************************************/
    if (Txfp->GetAmType() == TYPE_AM_MAP && Mode == MODE_DELETE)
      Txfp = new(g) MAPFAM((PDOSDEF)To_Def);
#if defined(ZIP_SUPPORT)
    else if (Txfp->GetAmType() == TYPE_AM_ZIP)
      Txfp = new(g) ZIPFAM((PDOSDEF)To_Def);
#endif   // ZIP_SUPPORT
    else // if (Txfp->GetAmType() != TYPE_AM_DOS)    ???
      Txfp = new(g) DOSFAM((PDOSDEF)To_Def);

    Txfp->SetTdbp(this);
    } // endif Mode

  /*********************************************************************/
  /*  Open according to logical input/output mode required.            */
  /*  Use conventionnal input/output functions.                        */
  /*  Treat files as binary in Delete mode (for line moving)           */
  /*********************************************************************/
  if (Txfp->OpenTableFile(g))
    return true;

  Use = USE_OPEN;       // Do it now in case we are recursively called

  /*********************************************************************/
  /*  Allocate the block filter tree if evaluation is possible.        */
  /*********************************************************************/
  To_BlkFil = InitBlockFilter(g, To_Filter);

  /*********************************************************************/
  /*  Allocate the line buffer plus a null character.                  */
  /*********************************************************************/
  To_Line = (char*)PlugSubAlloc(g, NULL, Lrecl + 1);

  if (Mode == MODE_INSERT) {
    // Spaces between fields must be filled with blanks
    memset(To_Line, ' ', Lrecl);
    To_Line[Lrecl] = '\0';
  } else
    memset(To_Line, 0, Lrecl + 1);

  if (trace)
    htrc("OpenDos: R%hd mode=%d To_Line=%p\n", Tdb_No, Mode, To_Line);

  if (SkipHeader(g))         // When called from CSV/FMT files
    return true;

  /*********************************************************************/
  /*  Reset statistics values.                                         */
  /*********************************************************************/
  num_read = num_there = num_eq[0] = num_eq[1] = 0;
  return false;
  } // end of OpenDB

/***********************************************************************/
/*  ReadDB: Data Base read routine for DOS access method.              */
/***********************************************************************/
int TDBDOS::ReadDB(PGLOBAL g)
  {
  if (trace > 1)
    htrc("DOS ReadDB: R%d Mode=%d key=%p link=%p Kindex=%p To_Line=%p\n",
          GetTdb_No(), Mode, To_Key_Col, To_Link, To_Kindex, To_Line);

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
        num_there++;
        return RC_OK;
      default:
        /***************************************************************/
        /*  Set the file position according to record to read.         */
        /***************************************************************/
        if (SetRecpos(g, recpos))
          return RC_FX;

        if (trace > 1)
          htrc("File position is now %d\n", GetRecpos());

        if (Mode == MODE_READ)
          /*************************************************************/
          /*  Defer physical reading until one column setting needs it */
          /*  as it can be a big saving on joins where no other column */
          /*  than the keys are used, so reading is unnecessary.       */
          /*************************************************************/
          if (Txfp->DeferReading())
            return RC_OK;

      } // endswitch recpos

    } // endif To_Kindex

  if (trace > 1)
    htrc(" ReadDB: this=%p To_Line=%p\n", this, To_Line);

  /*********************************************************************/
  /*  Now start the reading process.                                   */
  /*********************************************************************/
  return ReadBuffer(g);
  } // end of ReadDB

/***********************************************************************/
/*  PrepareWriting: Prepare the line to write.                         */
/***********************************************************************/
bool TDBDOS::PrepareWriting(PGLOBAL)
  {
  if (!Ftype && (Mode == MODE_INSERT || Txfp->GetUseTemp())) {
    char *p;

    /*******************************************************************/
    /*  Suppress trailing blanks.                                      */
    /*  Also suppress eventual null from last line.                    */
    /*******************************************************************/
    for (p = To_Line + Lrecl -1; p >= To_Line; p--)
      if (*p && *p != ' ')
        break;

    *(++p) = '\0';
    } // endif Mode

  return false;
  } // end of PrepareWriting

/***********************************************************************/
/*  WriteDB: Data Base write routine for DOS access method.            */
/***********************************************************************/
int TDBDOS::WriteDB(PGLOBAL g)
  {
  if (trace > 1)
    htrc("DOS WriteDB: R%d Mode=%d \n", Tdb_No, Mode);

  // Make the line to write
  if (PrepareWriting(g))
    return RC_FX;

  if (trace > 1)
    htrc("Write: line is='%s'\n", To_Line);

  // Now start the writing process
  return Txfp->WriteBuffer(g);
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for DOS (and FIX) access method.     */
/*  RC_FX means delete all. Nothing to do here (was done at open).     */
/***********************************************************************/
int TDBDOS::DeleteDB(PGLOBAL g, int irc)
  {
    return (irc == RC_FX) ? RC_OK : Txfp->DeleteRecords(g, irc);
  } // end of DeleteDB

/***********************************************************************/
/*  Data Base close routine for DOS access method.                     */
/***********************************************************************/
void TDBDOS::CloseDB(PGLOBAL g)
  {
  if (To_Kindex) {
    To_Kindex->Close();
    To_Kindex = NULL;
    } // endif

  Txfp->CloseTableFile(g, Abort);
  RestoreNrec();
  } // end of CloseDB

// ------------------------ DOSCOL functions ----------------------------

/***********************************************************************/
/*  DOSCOL public constructor (also called by MAPCOL).                 */
/***********************************************************************/
DOSCOL::DOSCOL(PGLOBAL g, PCOLDEF cdp, PTDB tp, PCOL cp, int i, PSZ am)
  : COLBLK(cdp, tp, i)
  {
  char *p;
  int   prec = Format.Prec;
  PTXF  txfp = ((PTDBDOS)tp)->Txfp;

  assert(cdp);

  if (cp) {
    Next = cp->GetNext();
    cp->SetNext(this);
  } else {
    Next = tp->GetColumns();
    tp->SetColumns(this);
  } // endif cprec

  // Set additional Dos access method information for column.
  Deplac = cdp->GetOffset();
  Long = cdp->GetLong();
  To_Val = NULL;
  Clustered = cdp->GetOpt();
  Sorted = (cdp->GetOpt() == 2) ? 1 : 0;
  Ndv = 0;                // Currently used only for XDB2
  Nbm = 0;                // Currently used only for XDB2
  Min = NULL;
  Max = NULL;
  Bmap = NULL;
  Dval = NULL;
  Buf = NULL;

  if (txfp->Blocked && Opt && (cdp->GetMin() || cdp->GetDval())) {
    int nblk = txfp->GetBlock();

    Clustered = (cdp->GetXdb2()) ? 2 : 1;
    Sorted = (cdp->GetOpt() > 1) ? 1 : 0;   // Currently ascending only

    if (Clustered == 1) {
      Min = AllocValBlock(g, cdp->GetMin(), Buf_Type, nblk, Long, prec);
      Max = AllocValBlock(g, cdp->GetMax(), Buf_Type, nblk, Long, prec);
    } else {        // Clustered == 2
      // Ndv is the number of distinct values in Dval. Ndv and Nbm
      // may be 0 when optimizing because Ndval is not filled yet,
      // but the size of the passed Dval memory block is Ok.
      Ndv = cdp->GetNdv();
      Dval = AllocValBlock(g, cdp->GetDval(), Buf_Type, Ndv, Long, prec);

      // Bmap cannot be allocated when optimizing, we must know Nbm first
      if ((Nbm = cdp->GetNbm()))
        Bmap = AllocValBlock(g, cdp->GetBmap(), TYPE_INT, Nbm * nblk);

    } // endif Clustered

    } // endif Opt

  OldVal = NULL;                  // Currently used only in MinMax
  Dsp = 0;
  Ldz = false;
  Nod = false;
  Dcm = -1;
  p = cdp->GetFmt();
  Buf = NULL;

  if (p && IsTypeNum(Buf_Type)) {
    // Formatted numeric value
    for (; p && *p && isalpha(*p); p++)
      switch (toupper(*p)) {
        case 'Z':                 // Have leading zeros
          Ldz = true;
          break;
        case 'N':                 // Have no decimal point
          Nod = true;
          break;
        case 'D':                 // Decimal separator
          Dsp = *(++p);
          break;
        } // endswitch p

    // Set number of decimal digits
    Dcm = (*p) ? atoi(p) : GetScale();
    } // endif fmt

  if (trace)
    htrc(" making new %sCOL C%d %s at %p\n", am, Index, Name, this);

  } // end of DOSCOL constructor

/***********************************************************************/
/*  DOSCOL constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
DOSCOL::DOSCOL(DOSCOL *col1, PTDB tdbp) : COLBLK(col1, tdbp)
  {
  Deplac = col1->Deplac;
  Long = col1->Long;
  To_Val = col1->To_Val;
  Ldz = col1->Ldz;
  Dsp = col1->Dsp;
  Nod = col1->Nod;
  Dcm = col1->Dcm;
  OldVal = col1->OldVal;
  Buf = col1->Buf;
  Clustered = col1->Clustered;
  Sorted = col1->Sorted;
  Min = col1->Min;
  Max = col1->Max;
  Bmap = col1->Bmap;
  Dval = col1->Dval;
  Ndv = col1->Ndv;
  Nbm = col1->Nbm;
  } // end of DOSCOL copy constructor

/***********************************************************************/
/*  VarSize: This function tells UpdateDB whether or not the block     */
/*  optimization file must be redone if this column is updated, even   */
/*  it is not sorted or clustered. This applies to the last column of  */
/*  a variable length table that is blocked, because if it is updated  */
/*  using a temporary file, the block size may be modified.            */
/***********************************************************************/
bool DOSCOL::VarSize(void)
  {
  PTDBDOS tdbp = (PTDBDOS)To_Tdb;
  PTXF    txfp = tdbp->Txfp;

  if (Cdp && !Cdp->GetNext()               // Must be the last column
          && tdbp->Ftype == RECFM_VAR      // of a DOS variable length
          && txfp->Blocked                 // blocked table
          && txfp->GetUseTemp())           // using a temporary file.
    return true;
  else
    return false;

  } // end VarSize

/***********************************************************************/
/*  SetBuffer: prepare a column block for write operation.             */
/***********************************************************************/
bool DOSCOL::SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check)
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

  // Allocate the buffer used in WriteColumn for numeric columns
  if (!Buf && IsTypeNum(Buf_Type))
    Buf = (char*)PlugSubAlloc(g, NULL, MY_MAX(32, Long + Dcm + 1));

  // Because Colblk's have been made from a copy of the original TDB in
  // case of Update, we must reset them to point to the original one.
  if (To_Tdb->GetOrig())
    To_Tdb = (PTDB)To_Tdb->GetOrig();

  // Set the Column
  Status = (ok) ? BUF_EMPTY : BUF_NO;
  return false;
  } // end of SetBuffer

/***********************************************************************/
/*  ReadColumn: what this routine does is to access the last line      */
/*  read from the corresponding table, extract from it the field       */
/*  corresponding to this column and convert it to buffer type.        */
/***********************************************************************/
void DOSCOL::ReadColumn(PGLOBAL g)
  {
  char   *p = NULL;
  int     i, rc;
  int     field;
  double  dval;
  PTDBDOS tdbp = (PTDBDOS)To_Tdb;

  if (trace > 1)
    htrc(
      "DOS ReadColumn: col %s R%d coluse=%.4X status=%.4X buf_type=%d\n",
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
  field = Long;

  /*********************************************************************/
  /*  For a variable length file, check if the field exists.           */
  /*********************************************************************/
  if (tdbp->Ftype == RECFM_VAR && strlen(tdbp->To_Line) < (unsigned)Deplac)
    field = 0;
  else if (Dsp)
    for(i = 0; i < field; i++)
      if (p[i] == Dsp)
        p[i] = '.';

  switch (tdbp->Ftype) {
    case RECFM_VAR:
    case RECFM_FIX:            // Fixed length text file
    case RECFM_DBF:            // Fixed length DBase file
      if (Nod) switch (Buf_Type) {
        case TYPE_INT:
        case TYPE_SHORT:
        case TYPE_TINY:
        case TYPE_BIGINT:
          if (Value->SetValue_char(p, field - Dcm)) {
            sprintf(g->Message, "Out of range value for column %s at row %d",
                    Name, tdbp->RowNumber(g));
            PushWarning(g, tdbp);
            } // endif SetValue_char

          break;
        case TYPE_DOUBLE:
          Value->SetValue_char(p, field);
          dval = Value->GetFloatValue();

          for (i = 0; i < Dcm; i++)
            dval /= 10.0;

          Value->SetValue(dval);
          break;
        default:
          Value->SetValue_char(p, field);
          break;
        } // endswitch Buf_Type

      else
        if (Value->SetValue_char(p, field)) {
          sprintf(g->Message, "Out of range value for column %s at row %d",
                  Name, tdbp->RowNumber(g));
          PushWarning(g, tdbp);
          } // endif SetValue_char

      break;
    default:
      sprintf(g->Message, MSG(BAD_RECFM), tdbp->Ftype);
      longjmp(g->jumper[g->jump_level], 34);
    } // endswitch Ftype

  // Set null when applicable
  if (Nullable)
    Value->SetNull(Value->IsZero());

  } // end of ReadColumn

/***********************************************************************/
/*  WriteColumn: what this routine does is to access the last line     */
/*  read from the corresponding table, and rewrite the field           */
/*  corresponding to this column from the column buffer and type.      */
/***********************************************************************/
void DOSCOL::WriteColumn(PGLOBAL g)
  {
  char   *p, *p2, fmt[32];
  int     i, k, len, field;
  PTDBDOS tdbp = (PTDBDOS)To_Tdb;

  if (trace > 1)
    htrc("DOS WriteColumn: col %s R%d coluse=%.4X status=%.4X\n",
          Name, tdbp->GetTdb_No(), ColUse, Status);

  p = tdbp->To_Line + Deplac;

  if (trace > 1)
    htrc("Lrecl=%d deplac=%d int=%d\n", tdbp->Lrecl, Deplac, Long);

  field = Long;

  if (tdbp->Ftype == RECFM_VAR && tdbp->Mode == MODE_UPDATE) {
    len = (signed)strlen(tdbp->To_Line);

    if (tdbp->IsUsingTemp(g))
      // Because of eventual missing field(s) the buffer must be reset
      memset(tdbp->To_Line + len, ' ', tdbp->Lrecl - len);
    else
      // The size actually available must be recalculated
      field = MY_MIN(len - Deplac, Long);

    } // endif Ftype

  if (trace > 1)
    htrc("Long=%d field=%d coltype=%d colval=%p\n",
          Long, field, Buf_Type, Value);

  /*********************************************************************/
  /*  Get the string representation of Value according to column type. */
  /*********************************************************************/
  if (Value != To_Val)
    Value->SetValue_pval(To_Val, false);    // Convert the updated value

  /*********************************************************************/
  /*  This test is only useful for compressed(2) tables.               */
  /*********************************************************************/
  if (tdbp->Ftype != RECFM_BIN) {
    if (Ldz || Nod || Dcm >= 0) {
      switch (Buf_Type) {
        case TYPE_SHORT:
          strcpy(fmt, (Ldz) ? "%0*hd" : "%*.hd");
          i = 0;

          if (Nod)
            for (; i < Dcm; i++)
              strcat(fmt, "0");

          len = sprintf(Buf, fmt, field - i, Value->GetShortValue());
          break;
        case TYPE_INT:
          strcpy(fmt, (Ldz) ? "%0*d" : "%*.d");
          i = 0;

          if (Nod)
            for (; i < Dcm; i++)
              strcat(fmt, "0");

          len = sprintf(Buf, fmt, field - i, Value->GetIntValue());
          break;
        case TYPE_TINY:
          strcpy(fmt, (Ldz) ? "%0*d" : "%*.d");
          i = 0;

          if (Nod)
            for (; i < Dcm; i++)
              strcat(fmt, "0");

          len = sprintf(Buf, fmt, field - i, Value->GetTinyValue());
          break;
        case TYPE_DOUBLE:
        case TYPE_DECIM:
          strcpy(fmt, (Ldz) ? "%0*.*lf" : "%*.*lf");
          sprintf(Buf, fmt, field + ((Nod && Dcm) ? 1 : 0),
                  Dcm, Value->GetFloatValue());
          len = strlen(Buf);

          if (Nod && Dcm)
            for (i = k = 0; i < len; i++, k++)
              if (Buf[i] != ' ') {
                if (Buf[i] == '.')
                  k++;

                Buf[i] = Buf[k];
                } // endif Buf(i)

          len = strlen(Buf);
          break;
        default:
          sprintf(g->Message, "Invalid field format for column %s", Name);
          longjmp(g->jumper[g->jump_level], 31);
        } // endswitch BufType

      p2 = Buf;
    } else                 // Standard CONNECT format
      p2 = Value->ShowValue(Buf, field);

    if (trace)
      htrc("new length(%p)=%d\n", p2, strlen(p2));

    if ((len = strlen(p2)) > field) {
      sprintf(g->Message, MSG(VALUE_TOO_LONG), p2, Name, field);
      longjmp(g->jumper[g->jump_level], 31);
    } else if (Dsp)
      for (i = 0; i < len; i++)
        if (p2[i] == '.')
          p2[i] = Dsp; 

    if (trace > 1)
      htrc("buffer=%s\n", p2);

    /*******************************************************************/
    /*  Updating must be done only when not in checking pass.          */
    /*******************************************************************/
    if (Status) {
      memset(p, ' ', field);
      memcpy(p, p2, len);

      if (trace > 1)
        htrc(" col write: '%.*s'\n", len, p);

      } // endif Use

  } else    // BIN compressed table
    /*******************************************************************/
    /*  Check if updating is Ok, meaning col value is not too long.    */
    /*  Updating to be done only during the second pass (Status=true)  */
    /*******************************************************************/
    if (Value->GetBinValue(p, Long, Status)) {
      sprintf(g->Message, MSG(BIN_F_TOO_LONG),
                          Name, Value->GetSize(), Long);
      longjmp(g->jumper[g->jump_level], 31);
      } // endif

  } // end of WriteColumn

/***********************************************************************/
/*  SetMinMax: Calculate minimum and maximum values for one block.     */
/*  Note: TYPE_STRING is stored and processed with zero ended strings  */
/*  to be matching the way the FILTER Eval function processes them.    */
/***********************************************************************/
bool DOSCOL::SetMinMax(PGLOBAL g)
  {
  PTDBDOS tp = (PTDBDOS)To_Tdb;

  ReadColumn(g);           // Extract column value from current line

  if (CheckSorted(g))
    return true;

  if (!tp->Txfp->CurNum) {
    Min->SetValue(Value, tp->Txfp->CurBlk);
    Max->SetValue(Value, tp->Txfp->CurBlk);
  } else {
    Min->SetMin(Value, tp->Txfp->CurBlk);
    Max->SetMax(Value, tp->Txfp->CurBlk);
  } // endif CurNum

  return false;
  } // end of SetMinMax

/***********************************************************************/
/*  SetBitMap: Calculate the bit map of existing values in one block.  */
/*  Note: TYPE_STRING is processed with zero ended strings             */
/*  to be matching the way the FILTER Eval function processes them.    */
/***********************************************************************/
bool DOSCOL::SetBitMap(PGLOBAL g)
  {
  int     i, m, n;
  uint   *bmp;
  PTDBDOS tp = (PTDBDOS)To_Tdb;
  PDBUSER dup = PlgGetUser(g);

  n = tp->Txfp->CurNum;
  bmp = (uint*)Bmap->GetValPtr(Nbm * tp->Txfp->CurBlk);

  // Extract column value from current line
  ReadColumn(g);

  if (CheckSorted(g))
    return true;

  if (!n)                      // New block
    for (m = 0; m < Nbm; m++)
      bmp[m] = 0;             // Reset the new bit map

  if ((i = Dval->Find(Value)) < 0) {
    char buf[32];

    sprintf(g->Message, MSG(DVAL_NOTIN_LIST),
      Value->GetCharString(buf), Name);
    return true;
  } else if (i >= dup->Maxbmp) {
    sprintf(g->Message, MSG(OPT_LOGIC_ERR), i);
    return true;
  } else {
    m = i / MAXBMP;
#if defined(_DEBUG)
    assert (m < Nbm);
#endif   // _DEBUG
    bmp[m] |= (1 << (i % MAXBMP));
  } // endif's i

  return false;
  } // end of SetBitMap

/***********************************************************************/
/*  Checks whether a column declared as sorted is sorted indeed.       */
/***********************************************************************/
bool DOSCOL::CheckSorted(PGLOBAL g)
  {
  if (Sorted)
    if (OldVal) {
      // Verify whether this column is sorted all right
      if (OldVal->CompareValue(Value) > 0) {
        // Column is no more in ascending order
        sprintf(g->Message, MSG(COL_NOT_SORTED), Name, To_Tdb->GetName());
        Sorted = false;
        return true;
      } else
        OldVal->SetValue_pval(Value);

    } else
      OldVal = AllocateValue(g, Value);

  return false;
  } // end of CheckSorted

/***********************************************************************/
/*  AddDistinctValue: Check whether this value already exist in the    */
/*  list and if not add it to the distinct values list.                */
/***********************************************************************/
bool DOSCOL::AddDistinctValue(PGLOBAL g)
  {
  bool found = false;
  int  i, m, n;

  ReadColumn(g);           // Extract column value from current line

  // Perhaps a better algorithm can be used when Ndv gets bigger
  // Here we cannot use Find because we must get the index of where
  // to insert a new value if it is not found in the array.
  for (n = 0; n < Ndv; n++) {
    m = Dval->CompVal(Value, n);

    if (m > 0)
      continue;
    else if (!m)
      found = true;        // Already there

    break;
    } // endfor n

  if (!found) {
    // Check whether we have room for an additional value
    if (Ndv == Freq) {
      // Too many values because of wrong Freq setting
      sprintf(g->Message, MSG(BAD_FREQ_SET), Name);
      return true;
      } // endif Ndv

    // New value, add it to the list before the nth value
    Dval->SetNval(Ndv + 1);

    for (i = Ndv; i > n; i--)
      Dval->Move(i - 1, i);

    Dval->SetValue(Value, n);
    Ndv++;
    } // endif found

  return false;
  } // end of AddDistinctValue

/***********************************************************************/
/*  Make file output of a Dos column descriptor block.                 */
/***********************************************************************/
void DOSCOL::Print(PGLOBAL g, FILE *f, uint n)
  {
  COLBLK::Print(g, f, n);
  } // end of Print

/* ------------------------------------------------------------------- */

