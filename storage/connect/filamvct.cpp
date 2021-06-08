/*********** File AM Vct C++ Program Source Code File (.CPP) ***********/
/* PROGRAM NAME: FILAMVCT                                              */
/* -------------                                                       */
/*  Version 2.6                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2020    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the VCT file access method classes.               */
/*  Added in version 2:                                                */
/*  - Split Vec format.                                                */
/*  - Partial delete.                                                  */
/*  - Use of tempfile for update.                                      */
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
#endif   // __BORLAND__
//#include <windows.h>
#include <sys/stat.h>
#else   // !_WIN32
#if defined(UNIX)
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#define NO_ERROR 0
#else   // !UNIX
#include <io.h>
#endif  // !UNIX
#include <fcntl.h>
#endif  // !_WIN32

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  tabdos.h    is header containing the TABDOS class declarations.    */
/***********************************************************************/
#include "global.h"
#include "osutil.h"            // Unuseful for WINDOWS
#include "plgdbsem.h"
#include "valblk.h"
#include "filamfix.h"
#include "tabdos.h"
#include "tabvct.h"
#include "maputil.h"
#include "filamvct.h"

#ifndef INVALID_SET_FILE_POINTER
#define INVALID_SET_FILE_POINTER  ((DWORD)-1)
#endif

extern int num_read, num_there;                          // Statistics
static int num_write;

/***********************************************************************/
/*  Header containing block info for not split VEC tables.             */
/*  Block and last values can be calculated from NumRec and Nrec.      */
/*  This is better than directly storing Block and Last because it     */
/*  make possible to use the same file with tables having a different  */
/*  block size value (Element -> Nrec)                                 */
/*  Note: can be in a separate file if header=1 or a true header (2)   */
/***********************************************************************/
typedef struct _vecheader {
//int Block;              /* The number of used blocks                 */
//int Last;               /* The number of used records in last block  */
  int MaxRec;             /* Max number of records (True vector format)*/
  int NumRec;             /* Number of valid records in the table      */
  } VECHEADER;

/***********************************************************************/
/*  Char VCT column blocks are right filled with blanks (blank = true) */
/*  Conversion of block values allowed conditionally for insert only.  */
/***********************************************************************/
PVBLK AllocValBlock(PGLOBAL, void *, int, int, int, int,
                    bool check = true, bool blank = true, bool un = false);

/* -------------------------- Class VCTFAM --------------------------- */

/***********************************************************************/
/*  Implementation of the VCTFAM class.                                */
/***********************************************************************/
VCTFAM::VCTFAM(PVCTDEF tdp) : FIXFAM((PDOSDEF)tdp)
  {
  Last = tdp->GetLast();
  MaxBlk = (tdp->GetEstimate() > 0) ?
          ((tdp->GetEstimate() - 1) / Nrec + 1) : 0;
  NewBlock = NULL;
  AddBlock = false;
  Split = false;

  if ((Header = (MaxBlk) ? tdp->Header : 0))
    Block = Last = -1;

  Bsize = Nrec;
  CurNum = Nrec - 1;
  Colfn = NULL;
  Tempat = NULL;
  Clens = NULL;
  Deplac = NULL;
  Isnum = NULL;
  Ncol = 0;
  } // end of VCTFAM standard constructor

VCTFAM::VCTFAM(PVCTFAM txfp) : FIXFAM(txfp)
  {
  MaxBlk = txfp->MaxBlk;
  NewBlock = NULL;
  AddBlock = false;
  Split = txfp->Split;
  Header = txfp->Header;
  Bsize = txfp->Bsize;
  Colfn = txfp->Colfn;
  Tempat = txfp->Tempat;
  Clens = txfp->Clens;
  Deplac = txfp->Deplac;
  Isnum = txfp->Isnum;
  Ncol = txfp->Ncol;
  } // end of VCTFAM copy constructor

/***********************************************************************/
/*  VCT GetFileLength: returns file size in number of bytes.           */
/*  This function is here to be accessible by VECFAM and VMPFAM.       */
/***********************************************************************/
int VCTFAM::GetFileLength(PGLOBAL g)
  {
  if (Split) {
    // Get the total file length
    char filename[_MAX_PATH];
    PCSZ savfile = To_File;
    int  i, len = 0;

    //  Initialize the array of file structures
    if (!Colfn) {
      // Prepare the column file name pattern and set Ncol
      Colfn = (char*)PlugSubAlloc(g, NULL, _MAX_PATH);
      Ncol = ((PVCTDEF)Tdbp->GetDef())->MakeFnPattern(Colfn);
      } // endif Colfn

    To_File = filename;

    for (i = 0; i < Ncol; i++) {
      sprintf(filename, Colfn, i+1);
      len += TXTFAM::GetFileLength(g);
      } // endfor i

    To_File = savfile;
    return len;
  } else
    return TXTFAM::GetFileLength(g);

  } // end of GetFileLength

/***********************************************************************/
/*  Reset read/write position values.                                  */
/***********************************************************************/
void VCTFAM::Reset(void)
  {
  FIXFAM::Reset();
  NewBlock = NULL;
  AddBlock = false;
  CurNum = Nrec - 1;
  } // end of Reset

/***********************************************************************/
/*  Get the Headlen, Block and Last info from the file header.         */
/***********************************************************************/
int VCTFAM::GetBlockInfo(PGLOBAL g)
  {
  char      filename[_MAX_PATH];
  int       h, k, n;
  VECHEADER vh;

  if (Header < 1 || Header > 3 || !MaxBlk) {
    sprintf(g->Message, "Invalid header value %d", Header);
    return -1;
  } else
    n = (Header == 1) ? (int)sizeof(VECHEADER) : 0;

  PlugSetPath(filename, To_File, Tdbp->GetPath());

  if (Header == 2)
    strcat(PlugRemoveType(filename, filename), ".blk");

  if ((h = global_open(g, MSGID_CANNOT_OPEN, filename, O_RDONLY)) == -1
      || !_filelength(h)) {
    // Consider this is a void table
    Last = Nrec;
    Block = 0;

    if (h != -1)
      close(h);

    return n;
  } else if (Header == 3)
    k = lseek(h, -(int)sizeof(VECHEADER), SEEK_END);

  if ((k = read(h, &vh, sizeof(vh))) != sizeof(vh)) {
    sprintf(g->Message, "Error reading header file %s", filename);
    n = -1;
  } else if (MaxBlk * Nrec != vh.MaxRec) {
    sprintf(g->Message, "MaxRec=%d doesn't match MaxBlk=%d Nrec=%d",
                        vh.MaxRec, MaxBlk, Nrec);
    n = -1;
  } else {
    Block = (vh.NumRec > 0) ? (vh.NumRec + Nrec - 1) / Nrec : 0;
    Last  = (vh.NumRec + Nrec - 1) % Nrec + 1;
  } // endif s

  close(h);
  return n;
  } // end of GetBlockInfo

/***********************************************************************/
/*  Get the Headlen, Block and Last info from the file header.         */
/***********************************************************************/
bool VCTFAM::SetBlockInfo(PGLOBAL g)
  {
  char      filename[_MAX_PATH];
  bool      rc = false;
  size_t    n;
  VECHEADER vh;
  FILE     *s;

  PlugSetPath(filename, To_File, Tdbp->GetPath());

  if (Header != 2) {
    if (Stream) {
      s = Stream;

      if (Header == 1)
        /*k =*/ fseek(s, 0, SEEK_SET);

    } else
      s= global_fopen(g, MSGID_CANNOT_OPEN, filename, "r+b");

  } else {      // Header == 2
    strcat(PlugRemoveType(filename, filename), ".blk");
    s= global_fopen(g, MSGID_CANNOT_OPEN, filename, "wb");
  } // endif Header

  if (!s) {
    sprintf(g->Message, "Error opening header file %s", filename);
    return true;
  } else if (Header == 3)
    /*k =*/ fseek(s, -(int)sizeof(VECHEADER), SEEK_END);

  vh.MaxRec = MaxBlk * Bsize;
  vh.NumRec = (Block - 1) * Nrec + Last;

  if ((n = fwrite(&vh, sizeof(vh), 1, s)) != 1) {
    sprintf(g->Message, "Error writing header file %s", filename);
    rc = true;
    } // endif fread

  if (Header == 2 || !Stream)
    fclose(s);

  return rc;
  } // end of SetBlockInfo

/***********************************************************************/
/*  Use BlockTest to reduce the table estimated size.                  */
/***********************************************************************/
int VCTFAM::MaxBlkSize(PGLOBAL g, int)
  {
  int rc = RC_OK, savcur = CurBlk;
  int size;

  // Roughly estimate the table size as the sum of blocks
  // that can contain good rows
  for (size = 0, CurBlk = 0; CurBlk < Block; CurBlk++)
    if ((rc = Tdbp->TestBlock(g)) == RC_OK)
      size += (CurBlk == Block - 1) ? Last : Nrec;
    else if (rc == RC_EF)
      break;

  CurBlk = savcur;
  return size;
  } // end of MaxBlkSize

/***********************************************************************/
/*  VCT Cardinality: returns table cardinality in number of rows.      */
/*  This function can be called with a null argument to test the       */
/*  availability of Cardinality implementation (1 yes, 0 no).          */
/***********************************************************************/
int VCTFAM::Cardinality(PGLOBAL g)
  {
  if (!g)
    return 1;

  if (Block < 0)
    if (Split) {
      // Separate column files and no pre setting of Block and Last
      // This allows to see a table modified externally, but Block
      // and Last must be set from the file cardinality.
      // Only happens when called by sub classes.
      char    filename[_MAX_PATH];
      PCSZ    savfn = To_File;
      int     len, clen, card = -1;
      PCOLDEF cdp = Tdbp->GetDef()->GetCols();

      if (!Colfn) {
        // Prepare the column file name pattern
        Colfn = (char*)PlugSubAlloc(g, NULL, _MAX_PATH);
        Ncol = ((VCTDEF*)Tdbp->GetDef())->MakeFnPattern(Colfn);
        } // endif Colfn

      // Use the first column file to calculate the cardinality
      clen = cdp->GetClen();
      sprintf(filename, Colfn, 1);
      To_File = filename;
      len = TXTFAM::GetFileLength(g);
      To_File = savfn;

      if (len >= 0) {
        if (!(len % clen))
          card = len / clen;           // Fixed length file
        else
          sprintf(g->Message, MSG(NOT_FIXED_LEN), To_File, len, clen);

      if (trace(1))
        htrc(" Computed max_K=%d Filen=%d Clen=%d\n", card, len, clen);

      } else
        card = 0;

      // Set number of blocks for later use
      Block = (card > 0) ? (card + Nrec - 1) / Nrec : 0;
      Last = (card + Nrec - 1) % Nrec + 1;
      return card;
    } else {
      // Vector table having Block and Last info in a Header (file)
      if ((Headlen = GetBlockInfo(g)) < 0)
        return -1;             // Error

    } // endif split

  return (Block) ? ((Block - 1) * Nrec + Last) : 0;
  } // end of Cardinality

/***********************************************************************/
/*  GetRowID: return the RowID of last read record.                    */
/***********************************************************************/
int VCTFAM::GetRowID(void)
  {
  return 1 + ((CurBlk < Block) ? CurNum + Nrec * CurBlk
                               : (Block - 1) * Nrec + Last);
  } // end of GetRowID

/***********************************************************************/
/*  VCT Create an empty file for Vector formatted tables.              */
/***********************************************************************/
bool VCTFAM::MakeEmptyFile(PGLOBAL g, PCSZ fn)
  {
  // Vector formatted file: this will create an empty file of the
  // required length if it does not exists yet.
  char filename[_MAX_PATH], c = 0;
  int  h, n;

  PlugSetPath(filename, fn, Tdbp->GetPath());
#if defined(_WIN32)
  h= global_open(g, MSGID_OPEN_EMPTY_FILE, filename, _O_CREAT | _O_WRONLY, S_IREAD | S_IWRITE);
#else   // !_WIN32
  h= global_open(g, MSGID_OPEN_EMPTY_FILE, filename,  O_CREAT |  O_WRONLY, S_IREAD | S_IWRITE);
#endif  // !_WIN32

  if (h == -1)
    return true;

  n = (Header == 1 || Header == 3) ? sizeof(VECHEADER) : 0;

  if (lseek(h, n + MaxBlk * Nrec * Lrecl - 1, SEEK_SET) < 0)
    goto err;

  // This actually fills the empty file
  if (write(h, &c, 1) < 0)     
    goto err;

  close(h);
  return false;
  
 err:
  sprintf(g->Message, MSG(MAKE_EMPTY_FILE), To_File, strerror(errno));
  close(h);
  return true;
  } // end of MakeEmptyFile

/***********************************************************************/
/*  VCT Access Method opening routine.                                 */
/*  New method now that this routine is called recursively (last table */
/*  first in reverse order): index blocks are immediately linked to    */
/*  join block of next table if it exists or else are discarted.       */
/***********************************************************************/
bool VCTFAM::OpenTableFile(PGLOBAL g)
  {
  char    opmode[4], filename[_MAX_PATH];
  MODE    mode = Tdbp->GetMode();
  PDBUSER dbuserp = PlgGetUser(g);

  /*********************************************************************/
  /*  Update block info if necessary.                                  */
  /*********************************************************************/
  if (Block < 0)
    if ((Headlen = GetBlockInfo(g)) < 0)
      return true;

  /*********************************************************************/
  /*  Open according to input/output mode required.                    */
  /*********************************************************************/
  switch (mode) {
    case MODE_READ:
      strcpy(opmode, "rb");
      break;
    case MODE_DELETE:
      if (!Tdbp->GetNext()) {
        // Store the number of deleted lines
        DelRows = Cardinality(g);

        // This will delete the whole file
        strcpy(opmode, "wb");
        break;
        } // endif

      // Selective delete, pass thru
      /* fall through */
    case MODE_UPDATE:
      UseTemp = Tdbp->IsUsingTemp(g);
      strcpy(opmode, (UseTemp) ? "rb" : "r+b");
      break;
    case MODE_INSERT:
      if (MaxBlk) {
        if (!Block)
          if (MakeEmptyFile(g, To_File))
            return true;

        strcpy(opmode, "r+b");   // Required to update empty blocks
      } else if (!Block || Last == Nrec)
        strcpy(opmode, "ab");
      else
        strcpy(opmode, "r+b");   // Required to update the last block

      break;
    default:
      sprintf(g->Message, MSG(BAD_OPEN_MODE), mode);
      return true;
    } // endswitch Mode

  /*********************************************************************/
  /*  Use conventionnal input/output functions.                        */
  /*********************************************************************/
  PlugSetPath(filename, To_File, Tdbp->GetPath());

  if (!(Stream = PlugOpenFile(g, filename, opmode))) {
    if (trace(1))
      htrc("%s\n", g->Message);

    return (mode == MODE_READ && errno == ENOENT)
            ? PushWarning(g, Tdbp) : true;
    } // endif Stream

  if (trace(1))
    htrc("File %s is open in mode %s\n", filename, opmode);

  To_Fb = dbuserp->Openlist;     // Keep track of File block

  if (!strcmp(opmode, "wb"))
    // This will stop the process by
    // causing GetProgMax to return 0.
    return ResetTableSize(g, 0, Nrec);

  num_read = num_there = num_write = 0;

  //  Allocate the table and column block buffer
  return AllocateBuffer(g);
  } // end of OpenTableFile

/***********************************************************************/
/*  Allocate the block buffers for columns used in the query.          */
/***********************************************************************/
bool VCTFAM::AllocateBuffer(PGLOBAL g)
  {
  MODE    mode = Tdbp->GetMode();
  PDOSDEF defp = (PDOSDEF)Tdbp->GetDef();
  PCOLDEF cdp;
  PVCTCOL cp = (PVCTCOL)Tdbp->GetColumns();

  if (mode == MODE_INSERT) {
    bool chk = PlgGetUser(g)->Check & CHK_TYPE;

    NewBlock = (char*)PlugSubAlloc(g, NULL, Blksize);

    for (cdp = defp->GetCols(); cdp; cdp = cdp->GetNext())
      memset(NewBlock + Nrec * cdp->GetPoff(),
            (IsTypeNum(cdp->GetType()) ? 0 : ' '),
                        Nrec * cdp->GetClen());

    for (; cp; cp = (PVCTCOL)cp->Next)
      cp->Blk = AllocValBlock(g, NewBlock + Nrec * cp->Deplac,
                              cp->Buf_Type, Nrec, cp->Format.Length,
                              cp->Format.Prec, chk, true, 
				                      cp->IsUnsigned());

    return InitInsert(g);    // Initialize inserting
  } else {
    if (UseTemp || mode == MODE_DELETE) {
      // Allocate all that is needed to move lines
      int i = 0, n = (MaxBlk) ? MaxBlk : 1;

      if (!Ncol)
        for (cdp = defp->GetCols(); cdp; cdp = cdp->GetNext())
          Ncol++;

      Clens = (int*)PlugSubAlloc(g, NULL, Ncol * sizeof(int));
      Deplac = (int*)PlugSubAlloc(g, NULL, Ncol * sizeof(int));
      Isnum = (bool*)PlugSubAlloc(g, NULL, Ncol * sizeof(bool));

      for (cdp = defp->GetCols(); cdp; i++, cdp = cdp->GetNext()) {
        Clens[i] = cdp->GetClen();
        Deplac[i] = Headlen + cdp->GetPoff() * n * Nrec;
        Isnum[i] = IsTypeNum(cdp->GetType());
        Buflen = MY_MAX(Buflen, cdp->GetClen());
        } // endfor cdp

      if (!UseTemp || MaxBlk) {
        Buflen *= Nrec;
        To_Buf = (char*)PlugSubAlloc(g, NULL, Buflen);
      } else
        NewBlock = (char*)PlugSubAlloc(g, NULL, Blksize);

      } // endif mode

    for (; cp; cp = (PVCTCOL)cp->Next)
      if (!cp->IsSpecial())            // Not a pseudo column
        cp->Blk = AllocValBlock(g, NULL, cp->Buf_Type, Nrec,
                                cp->Format.Length, cp->Format.Prec,
				                        true, true, cp->IsUnsigned());

  } //endif mode

  return false;
  } // end of AllocateBuffer

/***********************************************************************/
/*  Do initial action when inserting.                                  */
/***********************************************************************/
bool VCTFAM::InitInsert(PGLOBAL g)
{
	bool rc = false;

  // We come here in MODE_INSERT only
  if (Last == Nrec) {
    CurBlk = Block;
    CurNum = 0;
    AddBlock = !MaxBlk;
  } else {
    PVCTCOL cp = (PVCTCOL)Tdbp->GetColumns();

    // The starting point must be at the end of file as for append.
    CurBlk = Block - 1;
    CurNum = Last;

		try {
			// Last block must be updated by new values
			for (; cp; cp = (PVCTCOL)cp->Next)
				cp->ReadBlock(g);

		} catch (int n) {
			if (trace(1))
				htrc("Exception %d: %s\n", n, g->Message);
			rc = true;
		} catch (const char *msg) {
			strcpy(g->Message, msg);
			rc = true;
		} // end catch

  } // endif Last

	if (!rc)
    // We are not currently using a temporary file for Insert
    T_Stream = Stream;

  return rc;
  } // end of InitInsert

/***********************************************************************/
/*  ReadBuffer: Read one line for a VCT file.                          */
/***********************************************************************/
int VCTFAM::ReadBuffer(PGLOBAL g)
  {
  int  rc = RC_OK;
  MODE mode = Tdbp->GetMode();

  if (Placed)
    Placed = false;
  else if ((++CurNum) >= ((CurBlk < Block - 1) ? Nrec : Last)) {
    /*******************************************************************/
    /*  New block.                                                     */
    /*******************************************************************/
    CurNum = 0;

   next:
    if (++CurBlk == Block)
      return RC_EF;                        // End of file

    /*******************************************************************/
    /*  Before reading a new block, check whether block optimizing     */
    /*  can be done, as well as for join as for local filtering.       */
    /*******************************************************************/
    switch (Tdbp->TestBlock(g)) {
      case RC_EF:
        return RC_EF;
      case RC_NF:
        goto next;
      } // endswitch rc

    num_there++;
    } // endif CurNum

  if (OldBlk != CurBlk) {
    if (mode == MODE_UPDATE) {
      /*****************************************************************/
      /*  Flush the eventually modified column buffers in old blocks   */
      /*  and read the blocks to modify attached to Set columns.       */
      /*****************************************************************/
      if (MoveLines(g))               // For VECFAM
        return RC_FX;

      for (PVCTCOL colp = (PVCTCOL)Tdbp->GetSetCols();
                   colp; colp = (PVCTCOL)colp->Next) {
        colp->WriteBlock(g);
        colp->ReadBlock(g);
        } // endfor colp

      } // endif mode

    OldBlk = CurBlk;             // Last block actually read
    } // endif oldblk

  if (trace(1))
    htrc(" Read: CurNum=%d CurBlk=%d rc=%d\n", CurNum, CurBlk, RC_OK);

  return rc;
  } // end of ReadBuffer

/***********************************************************************/
/*  Data Base write routine for VCT access method.                     */
/***********************************************************************/
int VCTFAM::WriteBuffer(PGLOBAL g)
  {
  if (trace(1))
    htrc("VCT WriteBuffer: R%d Mode=%d CurNum=%d CurBlk=%d\n",
          Tdbp->GetTdb_No(), Tdbp->GetMode(), CurNum, CurBlk);

  if (Tdbp->GetMode() == MODE_UPDATE) {
    // Mode Update is done in ReadDB, we just initialize it here
    if (!T_Stream) {
      if (UseTemp) {
        if (OpenTempFile(g))
          return RC_FX;

        // Most of the time, not all table columns are updated.
        // This why we must completely pre-fill the temporary file.
        Fpos = (MaxBlk) ? (Block - 1) * Nrec + Last
                        : Block * Nrec;    // To write last lock

        if (MoveIntermediateLines(g))
          return RC_FX;

      } else
        T_Stream = Stream;

      } // endif T_Stream

  } else {
    // Mode Insert
    if (MaxBlk && CurBlk == MaxBlk) {
      strcpy(g->Message, MSG(TRUNC_BY_ESTIM));
      return RC_EF;       // Too many lines for vector formatted table
      } // endif MaxBlk

    if (Closing || ++CurNum == Nrec) {
      PVCTCOL cp = (PVCTCOL)Tdbp->GetColumns();

      if (!AddBlock) {
        // Write back the updated last block values
        for (; cp; cp = (PVCTCOL)cp->Next)
          cp->WriteBlock(g);

        if (!Closing && !MaxBlk) {
          // For VCT tables, future blocks must be added
          char filename[_MAX_PATH];

          // Close the file and reopen it in mode Insert
          fclose(Stream);
          PlugSetPath(filename, To_File, Tdbp->GetPath());

          if (!(Stream= global_fopen(g, MSGID_OPEN_MODE_STRERROR, filename, "ab"))) {
            Closing = true;          // Tell CloseDB of error
            return RC_FX;
            } // endif Stream

          AddBlock = true;
          } // endif Closing

      } else {
        // Here we must add a new block to the file
        if (Closing)
          // Reset the overwritten columns for last block extra records
          for (; cp; cp = (PVCTCOL)cp->Next)
            memset(NewBlock + Nrec * cp->Deplac + Last * cp->Clen,
                   (cp->Buf_Type == TYPE_STRING) ? ' ' : '\0',
                   (Nrec - Last) * cp->Clen);

        if ((size_t)Nrec !=
             fwrite(NewBlock, (size_t)Lrecl, (size_t)Nrec, Stream)) {
          sprintf(g->Message, MSG(WRITE_STRERROR), To_File, strerror(errno));
          return RC_FX;
          } // endif

      } // endif AddBlock

      if (!Closing) {
        CurBlk++;
        CurNum = 0;
        } // endif Closing

      } // endif Closing || CurNum

  } // endif Mode

  return RC_OK;
  } // end of WriteBuffer

/***********************************************************************/
/*  Data Base delete line routine for VCT access method.               */
/*  Note: lines are moved directly in the files (ooops...)             */
/*  Using temp file depends on the Check setting, false by default.    */
/***********************************************************************/
int VCTFAM::DeleteRecords(PGLOBAL g, int irc)
  {
  bool eof = false;

  if (trace(1))
    htrc("VCT DeleteDB: rc=%d UseTemp=%d Fpos=%d Tpos=%d Spos=%d\n",
          irc, UseTemp, Fpos, Tpos, Spos);

  if (irc != RC_OK) {
    /*******************************************************************/
    /*  EOF: position Fpos at the end-of-file position.                */
    /*******************************************************************/
    Fpos = (Block - 1) * Nrec + Last;

    if (trace(1))
      htrc("Fpos placed at file end=%d\n", Fpos);

    eof = UseTemp && !MaxBlk;
  } else     // Fpos is the Deleted line position
    Fpos = CurBlk * Nrec + CurNum;

  if (Tpos == Spos) {
    if (UseTemp) {
      /*****************************************************************/
      /*  Open the temporary file, Spos is at the beginning of file.   */
      /*****************************************************************/
      if (OpenTempFile(g))
        return RC_FX;

    } else {
      /*****************************************************************/
      /*  First line to delete. Move of eventual preceding lines is   */
      /*  not required here, just the setting of future Spos and Tpos. */
      /*****************************************************************/
      T_Stream = Stream;
      Spos = Tpos = Fpos;
    } // endif UseTemp

    } // endif Tpos == Spos

  /*********************************************************************/
  /*  Move any intermediate lines.                                     */
  /*********************************************************************/
  if (MoveIntermediateLines(g, &eof))
    return RC_FX;

  if (irc == RC_OK) {
    /*******************************************************************/
    /*  Reposition the file pointer and set Spos.                      */
    /*******************************************************************/
#ifdef _DEBUG
    assert(Spos == Fpos);
#endif
    Spos++;          // New start position is on next line

    if (trace(1))
      htrc("after: Tpos=%d Spos=%d\n", Tpos, Spos);

  } else {
    /*******************************************************************/
    /*  Last call after EOF has been reached.                          */
    /*  Update the Block and Last values.                              */
    /*******************************************************************/
    Block = (Tpos > 0) ? (Tpos + Nrec - 1) / Nrec : 0;
    Last = (Tpos + Nrec - 1) % Nrec + 1;

    if (!UseTemp) {   // The UseTemp case is treated in CloseTableFile
      if (!MaxBlk) {
        /***************************************************************/
        /*  Because the chsize functionality is only accessible with a */
        /*  system call we must close the file and reopen it with the  */
        /*  open function (_fopen for MS ??) this is still to be       */
        /*  checked for compatibility with Text files and other OS's.  */
        /***************************************************************/
        char filename[_MAX_PATH];
        int h;

        /*rc =*/ CleanUnusedSpace(g);           // Clean last block
        /*rc =*/ PlugCloseFile(g, To_Fb);
        Stream = NULL;                      // For SetBlockInfo
        PlugSetPath(filename, To_File, Tdbp->GetPath());

        if ((h= global_open(g, MSGID_OPEN_STRERROR, filename, O_WRONLY)) <= 0)
          return RC_FX;

        /***************************************************************/
        /*  Remove extra blocks.                                       */
        /***************************************************************/
#if defined(UNIX)
        if (ftruncate(h, (off_t)(Headlen + Block * Blksize))) {
          sprintf(g->Message, MSG(TRUNCATE_ERROR), strerror(errno));
          close(h);
          return RC_FX;
          } // endif
#else
        if (chsize(h, Headlen + Block * Blksize)) {
          sprintf(g->Message, MSG(CHSIZE_ERROR), strerror(errno));
          close(h);
          return RC_FX;
          } // endif
#endif

        close(h);

        if (trace(1))
          htrc("done, h=%d irc=%d\n", h, irc);

      } else
        // Clean the unused space in the file, this is required when
        // inserting again with a partial column list.
        if (CleanUnusedSpace(g))
          return RC_FX;

      if (ResetTableSize(g, Block, Last))
        return RC_FX;

      } // endif UseTemp

  } // endif irc

  return RC_OK;                                      // All is correct
  } // end of DeleteRecords

/***********************************************************************/
/*  Open a temporary file used while updating or deleting.             */
/***********************************************************************/
bool VCTFAM::OpenTempFile(PGLOBAL g)
  {
  PCSZ  opmode;
	char  tempname[_MAX_PATH];
	bool  rc = false;

  /*********************************************************************/
  /*  Open the temporary file, Spos is at the beginning of file.       */
  /*********************************************************************/
  PlugSetPath(tempname, To_File, Tdbp->GetPath());
  strcat(PlugRemoveType(tempname, tempname), ".t");

  if (MaxBlk) {
    if (MakeEmptyFile(g, tempname))
      return true;

    opmode = "r+b";
  } else
    opmode = "wb";

  if (!(T_Stream = PlugOpenFile(g, tempname, opmode))) {
    if (trace(1))
      htrc("%s\n", g->Message);

    rc = true;
  } else
    To_Fbt = PlgGetUser(g)->Openlist;

  return rc;
  } // end of OpenTempFile

/***********************************************************************/
/*  Move intermediate deleted or updated lines.                        */
/***********************************************************************/
bool VCTFAM::MoveIntermediateLines(PGLOBAL g, bool *b)
  {
  int    i, dep, off;
  int    n;
  bool   eof = (b) ? *b : false;
  size_t req, len;

  for (n = Fpos - Spos; n > 0 || eof; n -= req) {
    /*******************************************************************/
    /*  Non consecutive line to delete. Move intermediate lines.       */
    /*******************************************************************/
    if (!MaxBlk)
      req = (size_t)MY_MIN(n, Nrec - MY_MAX(Spos % Nrec, Tpos % Nrec));
    else
      req = (size_t)MY_MIN(n, Nrec);

    if (req) for (i = 0; i < Ncol; i++) {
      if (MaxBlk) {
        dep = Deplac[i];
        off = Spos * Clens[i];
      } else {
        if (UseTemp)
          To_Buf = NewBlock + Deplac[i] + (Tpos % Nrec) * Clens[i];

        dep = Deplac[i] + (Spos / Nrec) * Blksize;
        off = (Spos % Nrec) * Clens[i];
      } // endif MaxBlk

      if (fseek(Stream, dep + off, SEEK_SET)) {
        sprintf(g->Message, MSG(READ_SEEK_ERROR), strerror(errno));
        return true;
        } // endif

      len = fread(To_Buf, Clens[i], req, Stream);

      if (trace(1))
        htrc("after read req=%d len=%d\n", req, len);

      if (len != req) {
        sprintf(g->Message, MSG(DEL_READ_ERROR), (int) req, (int) len);
        return true;
        } // endif len

      if (!UseTemp || MaxBlk) {
        if (MaxBlk) {
          dep = Deplac[i];
          off = Tpos * Clens[i];
        } else {
          dep = Deplac[i] + (Tpos / Nrec) * Blksize;
          off = (Tpos % Nrec) * Clens[i];
        } // endif MaxBlk

        if (fseek(T_Stream, dep + off, SEEK_SET)) {
          sprintf(g->Message, MSG(WRITE_SEEK_ERR), strerror(errno));
          return true;
          } // endif

        if ((len = fwrite(To_Buf, Clens[i], req, T_Stream)) != req) {
          sprintf(g->Message, MSG(DEL_WRITE_ERROR), strerror(errno));
          return true;
          } // endif

        } // endif UseTemp

      if (trace(1))
        htrc("after write pos=%d\n", ftell(Stream));

      } // endfor i

    Tpos += (int)req;
    Spos += (int)req;

    if (UseTemp && !MaxBlk && (Tpos % Nrec == 0 || (eof && Spos == Fpos))) {
      // Write the full or last block to the temporary file
      if ((dep = Nrec - (Tpos % Nrec)) < Nrec)
        // Clean the last block in case of future insert,
        // must be done here because T_Stream was open in write only.
        for (i = 0; i < Ncol; i++) {
          To_Buf = NewBlock + Deplac[i] + (Tpos % Nrec) * Clens[i];
          memset(To_Buf, (Isnum[i]) ? 0 : ' ', dep * Clens[i]);
          } // endfor i

      // Write a new block in the temporary file
      len = (size_t)Blksize;

      if (fwrite(NewBlock, 1, len, T_Stream) != len) {
        sprintf(g->Message, MSG(DEL_WRITE_ERROR), strerror(errno));
        return true;
        } // endif

      if (Spos == Fpos)
        eof = false;

      } // endif UseTemp

    if (trace(1))
      htrc("loop: Tpos=%d Spos=%d\n", Tpos, Spos);

    } // endfor n

  return false;
  } // end of MoveIntermediateLines

/***********************************************************************/
/*  Clean deleted space in a VCT or Vec table file.                    */
/***********************************************************************/
bool VCTFAM::CleanUnusedSpace(PGLOBAL g)
  {
  int     i, dep;
  int    n;
  size_t  req, len;

  if (!MaxBlk) {
    /*******************************************************************/
    /*  Clean last block of the VCT table file.                        */
    /*******************************************************************/
    assert(!UseTemp);

    if (!(n = Nrec - Last))
      return false;

    dep = (Block - 1) * Blksize;
    req = (size_t)n;

    for (i = 0; i < Ncol; i++) {
      memset(To_Buf, (Isnum[i]) ? 0 : ' ', n * Clens[i]);

      if (fseek(Stream, dep + Deplac[i] + Last * Clens[i], SEEK_SET)) {
        sprintf(g->Message, MSG(WRITE_SEEK_ERR), strerror(errno));
        return true;
        } // endif

      if ((len = fwrite(To_Buf, Clens[i], req, Stream)) != req) {
        sprintf(g->Message, MSG(DEL_WRITE_ERROR), strerror(errno));
        return true;
        } // endif

      } // endfor i

  } else for (n = Fpos - Tpos; n > 0; n -= req) {
    /*******************************************************************/
    /*  Fill VEC file remaining lines with 0's.                        */
    /*  Note: this seems to work even column blocks have been made     */
    /*  with Blanks = true. Perhaps should it be set to false for VEC. */
    /*******************************************************************/
    req = (size_t)MY_MIN(n, Nrec);
    memset(To_Buf, 0, Buflen);

    for (i = 0; i < Ncol; i++) {
      if (fseek(T_Stream, Deplac[i] + Tpos * Clens[i], SEEK_SET)) {
        sprintf(g->Message, MSG(WRITE_SEEK_ERR), strerror(errno));
        return true;
        } // endif

      if ((len = fwrite(To_Buf, Clens[i], req, T_Stream)) != req) {
        sprintf(g->Message, MSG(DEL_WRITE_ERROR), strerror(errno));
        return true;
        } // endif

      } // endfor i

    Tpos += (int)req;
    } // endfor n

  return false;
  } // end of CleanUnusedSpace

/***********************************************************************/
/*  Data Base close routine for VCT access method.                     */
/***********************************************************************/
void VCTFAM::CloseTableFile(PGLOBAL g, bool abort)
  {
  int  rc = 0, wrc = RC_OK;
  MODE mode = Tdbp->GetMode();

  Abort = abort;

  if (mode == MODE_INSERT) {
    if (Closing)
      wrc = RC_FX;                  // Last write was in error
    else
      if (CurNum) {
        // Some more inserted lines remain to be written
        Last = CurNum;
        Block = CurBlk + 1;
        Closing = true;
        wrc = WriteBuffer(g);
      } else {
        Last = Nrec;
        Block = CurBlk;
        wrc = RC_OK;
      } // endif CurNum

    if (wrc != RC_FX) {
      rc = ResetTableSize(g, Block, Last);
    } else if (AddBlock) {
      // Last block was not written
      rc = ResetTableSize(g, CurBlk, Nrec);
			throw 44;
    } // endif

  } else if (mode == MODE_UPDATE) {
    // Write back to file any pending modifications
    for (PVCTCOL colp = (PVCTCOL)((PTDBVCT)Tdbp)->To_SetCols;
                 colp; colp = (PVCTCOL)colp->Next)
      colp->WriteBlock(g);

    if (UseTemp && T_Stream) {
      rc = RenameTempFile(g);

      if (Header) {
        // Header must be set because it was not set in temp file
        Stream = T_Stream = NULL;      // For SetBlockInfo
        rc = SetBlockInfo(g);
        } // endif Header

      } // endif UseTemp

  } else if (mode == MODE_DELETE && UseTemp && T_Stream) {
    if (MaxBlk)
      rc = CleanUnusedSpace(g);

    if ((rc = RenameTempFile(g)) != RC_FX) {
      Stream = T_Stream = NULL;      // For SetBlockInfo
      rc = ResetTableSize(g, Block, Last);
      } // endif rc

  } // endif's mode

  if (!(UseTemp && T_Stream))
    rc = PlugCloseFile(g, To_Fb);

  if (trace(1))
    htrc("VCT CloseTableFile: closing %s wrc=%d rc=%d\n",
          To_File, wrc, rc);

  Stream = NULL;
  } // end of CloseTableFile

/***********************************************************************/
/*  Data Base close routine for VCT access method.                     */
/***********************************************************************/
bool VCTFAM::ResetTableSize(PGLOBAL g, int block, int last)
  {
  bool rc = false;

  // Set Block and Last values for TDBVCT::MakeBlockValues
  Block = block;
  Last = last;

  if (!Split) {
    if (!Header) {
      // Update catalog values for Block and Last
      PVCTDEF defp = (PVCTDEF)Tdbp->GetDef();
      LPCSTR  name = Tdbp->GetName();

      defp->SetBlock(Block);
      defp->SetLast(Last);

      if (!defp->SetIntCatInfo("Blocks", Block) ||
          !defp->SetIntCatInfo("Last", Last)) {
        sprintf(g->Message, MSG(UPDATE_ERROR), "Header");
        rc = true;
        } // endif

    } else
      rc = SetBlockInfo(g);

    } // endif Split

  Tdbp->ResetSize();
  return rc;
  } // end of ResetTableSize

/***********************************************************************/
/*  Rewind routine for VCT access method.                              */
/***********************************************************************/
void VCTFAM::Rewind(void)
  {
  // In mode update we need to read Set Column blocks
  if (Tdbp->GetMode() == MODE_UPDATE)
    OldBlk = -1;

  // Initialize so block optimization is called for 1st block
  CurBlk = -1;
  CurNum = Nrec - 1;
//rewind(Stream);             will be placed by fseek
  } // end of Rewind

/***********************************************************************/
/*  ReadBlock: Read column values from current block.                  */
/***********************************************************************/
bool VCTFAM::ReadBlock(PGLOBAL g, PVCTCOL colp)
  {
  int     len;
  size_t  n;

  /*********************************************************************/
  /*  Calculate the offset and size of the block to read.              */
  /*********************************************************************/
  if (MaxBlk)                                 // True vector format
    len = Headlen + Nrec * (colp->Deplac * MaxBlk + colp->Clen * CurBlk);
  else                                        // Blocked vector format
    len = Nrec * (colp->Deplac + Lrecl * CurBlk);

  if (trace(1))
    htrc("len=%d Nrec=%d Deplac=%d Lrecl=%d CurBlk=%d maxblk=%d\n",
          len, Nrec, colp->Deplac, Lrecl, CurBlk, MaxBlk);

  if (fseek(Stream, len, SEEK_SET)) {
    sprintf(g->Message, MSG(FSEEK_ERROR), strerror(errno));
    return true;
    } // endif

  n = fread(colp->Blk->GetValPointer(), (size_t)colp->Clen,
                                        (size_t)Nrec, Stream);

  if (n != (size_t)Nrec) {
    if (errno == NO_ERROR)
      sprintf(g->Message, MSG(BAD_READ_NUMBER), (int) n, To_File);
    else
      sprintf(g->Message, MSG(READ_ERROR),
              To_File, strerror(errno));

    if (trace(1))
      htrc(" Read error: %s\n", g->Message);

    return true;
    } // endif

  if (trace(1))
    num_read++;

  return false;
  } // end of ReadBlock

/***********************************************************************/
/*  WriteBlock: Write back current column values for one block.        */
/*  Note: the test of Status is meant to prevent physical writing of   */
/*  the block during the checking loop in mode Update. It is set to    */
/*  BUF_EMPTY when reopening the table between the two loops.          */
/***********************************************************************/
bool VCTFAM::WriteBlock(PGLOBAL g, PVCTCOL colp)
  {
  int   len;
  size_t n;

  /*********************************************************************/
  /*  Calculate the offset and size of the block to write.             */
  /*********************************************************************/
  if (MaxBlk)                               // File has Vector format
    len = Headlen
        + Nrec * (colp->Deplac * MaxBlk + colp->Clen * colp->ColBlk);
  else                                      // Old VCT format
    len = Nrec * (colp->Deplac + Lrecl * colp->ColBlk);

  if (trace(1))
    htrc("modif=%d len=%d Nrec=%d Deplac=%d Lrecl=%d colblk=%d\n",
          Modif, len, Nrec, colp->Deplac, Lrecl, colp->ColBlk);

  if (fseek(T_Stream, len, SEEK_SET)) {
    sprintf(g->Message, MSG(FSEEK_ERROR), strerror(errno));
    return true;
    } // endif

  // Here Nrec was changed to CurNum in mode Insert,
  // this is the true number of records to write,
  // this also avoid writing garbage in the file for true vector tables.
  n = (Tdbp->GetMode() == MODE_INSERT) ? CurNum : Nrec;

  if (n != fwrite(colp->Blk->GetValPointer(),
                            (size_t)colp->Clen, n, T_Stream)) {
    sprintf(g->Message, MSG(WRITE_STRERROR),
            (UseTemp) ? To_Fbt->Fname : To_File, strerror(errno));

    if (trace(1))
      htrc("Write error: %s\n", strerror(errno));

    return true;
    } // endif

#if defined(UNIX)
  fflush(T_Stream); //NGC
#endif

#ifdef _DEBUG
  num_write++;
#endif

  return false;
  } // end of WriteBlock

/* -------------------------- Class VCMFAM --------------------------- */

/***********************************************************************/
/*  Implementation of the VCMFAM class.                                */
/***********************************************************************/
VCMFAM::VCMFAM(PVCTDEF tdp) : VCTFAM((PVCTDEF)tdp)
  {
  Memory = NULL;
  Memcol = NULL;
  } // end of VCMFAM standard constructor

VCMFAM::VCMFAM(PVCMFAM txfp) : VCTFAM(txfp)
  {
  Memory = txfp->Memory;
  Memcol = txfp->Memcol;
  } // end of VCMFAM copy constructor

/***********************************************************************/
/*  Mapped VCT Access Method opening routine.                          */
/*  New method now that this routine is called recursively (last table */
/*  first in reverse order): index blocks are immediately linked to    */
/*  join block of next table if it exists or else are discarted.       */
/***********************************************************************/
bool VCMFAM::OpenTableFile(PGLOBAL g)
  {
  char    filename[_MAX_PATH];
  size_t  len;
  MODE    mode = Tdbp->GetMode();
  PFBLOCK fp = NULL;
  PDBUSER dbuserp = (PDBUSER)g->Activityp->Aptr;

  /*********************************************************************/
  /*  Update block info if necessary.                                  */
  /*********************************************************************/
  if (Block < 0)
    if ((Headlen = GetBlockInfo(g)) < 0)
      return true;

  /*********************************************************************/
  /*  We used the file name relative to recorded datapath.             */
  /*********************************************************************/
  PlugSetPath(filename, To_File, Tdbp->GetPath());

  /*********************************************************************/
  /*  The whole file will be mapped so we can use it as if it were     */
  /*  entirely read into virtual memory.                               */
  /*  Firstly we check whether this file have been already mapped.     */
  /*********************************************************************/
  if (mode == MODE_READ) {
    for (fp = dbuserp->Openlist; fp; fp = fp->Next)
      if (fp->Type == TYPE_FB_MAP && !stricmp(fp->Fname, filename)
                     && fp->Count && fp->Mode == mode)
        break;

    if (trace(1))
      htrc("Mapping VCM file, fp=%p cnt=%d\n", fp, fp->Count);

  } else
    fp = NULL;

  if (fp) {
    /*******************************************************************/
    /*  File already mapped. Just increment use count and get pointer. */
    /*******************************************************************/
    fp->Count++;
    Memory = fp->Memory;
    len = fp->Length;
  } else {
    /*******************************************************************/
    /*  If required, delete the whole file if no filtering is implied. */
    /*******************************************************************/
    bool   del;
    HANDLE hFile;
    MEMMAP mm;
    MODE   mapmode = mode;

    if (mode == MODE_INSERT) {
      if (MaxBlk) {
        if (!Block)
          if (MakeEmptyFile(g, To_File))
            return true;

        // Inserting will be like updating the file
        mapmode = MODE_UPDATE;
      } else {
        strcpy(g->Message, "MAP Insert is for VEC Estimate tables only");
        return true;
      } // endif MaxBlk

      } // endif mode

    del = mode == MODE_DELETE && !Tdbp->GetNext();

    if (del) {
      DelRows = Cardinality(g);

      // This will stop the process by causing GetProgMax to return 0.
//    ResetTableSize(g, 0, Nrec);          must be done later
      } // endif del

    /*******************************************************************/
    /*  Create the mapping file object.                                */
    /*******************************************************************/
    hFile = CreateFileMap(g, filename, &mm, mapmode, del);

    if (hFile == INVALID_HANDLE_VALUE) {
      DWORD rc = GetLastError();

      if (!(*g->Message))
        sprintf(g->Message, MSG(OPEN_MODE_ERROR),
                "map", (int) rc, filename);

      if (trace(1))
        htrc("%s\n", g->Message);

      return (mode == MODE_READ && rc == ENOENT)
              ? PushWarning(g, Tdbp) : true;
      } // endif hFile

    /*******************************************************************/
    /*  Get the file size.                                             */
    /*******************************************************************/
		len = (size_t)mm.lenL;

		if (mm.lenH)
			len += ((size_t)mm.lenH * 0x000000001LL);

		Memory = (char *)mm.memory;

    if (!len) {             // Empty or deleted file
      CloseFileHandle(hFile);
      bool rc = ResetTableSize(g, 0, Nrec);
      return (mapmode == MODE_UPDATE) ? true : rc;
      } // endif len

    if (!Memory) {
      CloseFileHandle(hFile);
      sprintf(g->Message, MSG(MAP_VIEW_ERROR),
                          filename, GetLastError());
      return true;
      } // endif Memory

    if (mode != MODE_DELETE) {
      CloseFileHandle(hFile);                    // Not used anymore
      hFile = INVALID_HANDLE_VALUE;              // For Fblock
      } // endif Mode

    /*******************************************************************/
    /*  Link a Fblock. This make possible to reuse already opened maps */
    /*  and also to automatically unmap them in case of error g->jump. */
    /*  Note: block can already exist for previously closed file.      */
    /*******************************************************************/
    fp = (PFBLOCK)PlugSubAlloc(g, NULL, sizeof(FBLOCK));
    fp->Type = TYPE_FB_MAP;
    fp->Fname = PlugDup(g, filename);
    fp->Next = dbuserp->Openlist;
    dbuserp->Openlist = fp;
    fp->Count = 1;
    fp->Length = len;
    fp->Memory = Memory;
    fp->Mode = mode;
    fp->File = NULL;
    fp->Handle = hFile;                // Used for Delete
  } // endif fp

  To_Fb = fp;                               // Useful when closing

  if (trace(1))
    htrc("fp=%p count=%d MapView=%p len=%d Top=%p\n",
          fp, fp->Count, Memory, len);

  return AllocateBuffer(g);
  } // end of OpenTableFile

/***********************************************************************/
/*  Allocate the block buffers for columns used in the query.          */
/*  Give a dummy value (1) to prevent allocating the value block.     */
/*  It will be set pointing into the memory map of the file.           */
/*  Note: Memcol must be set for all columns because it can be used    */
/*  for set columns in Update. Clens values are used only in Delete.   */
/***********************************************************************/
bool VCMFAM::AllocateBuffer(PGLOBAL g)
  {
  int     m, i = 0;
  bool    b = Tdbp->GetMode() == MODE_DELETE;
  PVCTCOL cp;
  PCOLDEF cdp;
  PDOSDEF defp = (PDOSDEF)Tdbp->GetDef();

  // Calculate the number of columns
  if (!Ncol)
    for (cdp = defp->GetCols(); cdp; cdp = cdp->GetNext())
      Ncol++;

  // To store the start position of each column
  Memcol = (char**)PlugSubAlloc(g, NULL, Ncol * sizeof(char*));
  m = (MaxBlk) ? MaxBlk : 1;

  // We will need all column sizes and type for Delete
  if (b) {
    Clens = (int*)PlugSubAlloc(g, NULL, Ncol * sizeof(int));
    Isnum = (bool*)PlugSubAlloc(g, NULL, Ncol * sizeof(bool));
    } // endif b

  for (cdp = defp->GetCols(); i < Ncol; i++, cdp = cdp->GetNext()) {
    if (b) {
      Clens[i] = cdp->GetClen();
      Isnum[i] = IsTypeNum(cdp->GetType());
      } // endif b

    Memcol[i] = Memory + Headlen + cdp->GetPoff() * m * Nrec;
    } // endfor cdp

  for (cp = (PVCTCOL)Tdbp->GetColumns(); cp; cp = (PVCTCOL)cp->Next)
    if (!cp->IsSpecial()) {            // Not a pseudo column
      cp->Blk = AllocValBlock(g, (void*)1, cp->Buf_Type, Nrec,
                              cp->Format.Length, cp->Format.Prec,
				                      true, true, cp->IsUnsigned());
      cp->AddStatus(BUF_MAPPED);
      } // endif IsSpecial

  if (Tdbp->GetMode() == MODE_INSERT)
    return InitInsert(g);

  return false;
  } // end of AllocateBuffer

/***********************************************************************/
/*  Do initial action when inserting.                                  */
/***********************************************************************/
bool VCMFAM::InitInsert(PGLOBAL g)
{
  bool     rc = false;
  volatile PVCTCOL cp = (PVCTCOL)Tdbp->GetColumns();

  // We come here in MODE_INSERT only
  if (Last == Nrec) {
    CurBlk = Block;
    CurNum = 0;
    AddBlock = !MaxBlk;
  } else {
    // The starting point must be at the end of file as for append.
    CurBlk = Block - 1;
    CurNum = Last;
  } // endif Last

	try {
		// Initialize the column block pointer
		for (; cp; cp = (PVCTCOL)cp->Next)
			cp->ReadBlock(g);

	} catch (int n) {
	  if (trace(1))
		  htrc("Exception %d: %s\n", n, g->Message);
		rc = true;
  } catch (const char *msg) {
	  strcpy(g->Message, msg);
		rc = true;
  } // end catch

  return rc;
} // end of InitInsert

/***********************************************************************/
/*  Data Base write routine for VMP access method.                     */
/***********************************************************************/
int VCMFAM::WriteBuffer(PGLOBAL g)
  {
  if (trace(1))
    htrc("VCM WriteBuffer: R%d Mode=%d CurNum=%d CurBlk=%d\n",
          Tdbp->GetTdb_No(), Tdbp->GetMode(), CurNum, CurBlk);

  // Mode Update being done in ReadDB we process here Insert mode only.
  if (Tdbp->GetMode() == MODE_INSERT) {
    if (CurBlk == MaxBlk) {
      strcpy(g->Message, MSG(TRUNC_BY_ESTIM));
      return RC_EF;       // Too many lines for vector formatted table
      } // endif MaxBlk

    if (Closing || ++CurNum == Nrec) {
      PVCTCOL cp = (PVCTCOL)Tdbp->GetColumns();

      // Write back the updated last block values
      for (; cp; cp = (PVCTCOL)cp->Next)
        cp->WriteBlock(g);

      if (!Closing) {
        CurBlk++;
        CurNum = 0;

        // Re-initialize the column block pointer
        for (cp = (PVCTCOL)Tdbp->GetColumns(); cp; cp = (PVCTCOL)cp->Next)
          cp->ReadBlock(g);

        } // endif Closing

      } // endif Closing || CurNum

    } // endif Mode

  return RC_OK;
  } // end of WriteBuffer

/***********************************************************************/
/*  Data Base delete line routine for VMP access method.               */
/*  Lines between deleted lines are moved in the mapfile view.         */
/***********************************************************************/
int VCMFAM::DeleteRecords(PGLOBAL g, int irc)
  {
  if (trace(1))
    htrc("VCM DeleteDB: irc=%d tobuf=%p Tpos=%p Spos=%p\n",
                        irc, To_Buf, Tpos, Spos);

  if (irc != RC_OK) {
    /*******************************************************************/
    /*  EOF: position Fpos at the top of map position.                 */
    /*******************************************************************/
    Fpos = (Block - 1) * Nrec + Last;

    if (trace(1))
      htrc("Fpos placed at file top=%p\n", Fpos);

  } else     // Fpos is the Deleted line position
    Fpos = CurBlk * Nrec + CurNum;

  if (Tpos == Spos) {
    /*******************************************************************/
    /*  First line to delete. Move of eventual preceding lines is     */
    /*  not required here, just setting of future Spos and Tpos.       */
    /*******************************************************************/
    Tpos = Spos = Fpos;
  } else
    (void)MoveIntermediateLines(g);

  if (irc == RC_OK) {
    Spos = Fpos + 1;                               // New start position

    if (trace(1))
      htrc("after: Tpos=%p Spos=%p\n", Tpos, Spos);

  } else {
    /*******************************************************************/
    /*  Last call after EOF has been reached.                          */
    /*******************************************************************/
    int i, m, n;

    /*******************************************************************/
    /*  Reset the Block and Last values for TDBVCT::MakeBlockValues.   */
    /*******************************************************************/
    Block = (Tpos > 0) ? (Tpos + Nrec - 1) / Nrec : 0;
    Last = (Tpos + Nrec - 1) % Nrec + 1;

    if (!MaxBlk) {
      PFBLOCK fp = To_Fb;

      // Clean the unused part of the last block
      m = (Block - 1) * Blksize;
      n = Nrec - Last;

      for (i = 0; i < Ncol; i++)
        memset(Memcol[i] + m + Last * Clens[i],
            (Isnum[i]) ? 0 : ' ', n * Clens[i]);

      // We must Unmap the view and use the saved file handle
      // to put an EOF at the end of the last block of the file.
      CloseMemMap(fp->Memory, (size_t)fp->Length);
      fp->Count = 0;                            // Avoid doing it twice

      // Remove extra blocks
      n = Block * Blksize;

#if defined(_WIN32)
      DWORD drc = SetFilePointer(fp->Handle, n, NULL, FILE_BEGIN);

      if (drc == 0xFFFFFFFF) {
        sprintf(g->Message, MSG(FUNCTION_ERROR),
                            "SetFilePointer", GetLastError());
        CloseHandle(fp->Handle);
        return RC_FX;
        } // endif

      if (trace(1))
        htrc("done, Tpos=%p newsize=%d drc=%d\n", Tpos, n, drc);

      if (!SetEndOfFile(fp->Handle)) {
        sprintf(g->Message, MSG(FUNCTION_ERROR),
                            "SetEndOfFile", GetLastError());
        CloseHandle(fp->Handle);
        return RC_FX;
        } // endif

      CloseHandle(fp->Handle);
#else    // UNIX
      if (ftruncate(fp->Handle, (off_t)n)) {
        sprintf(g->Message, MSG(TRUNCATE_ERROR), strerror(errno));
        close(fp->Handle);
        return RC_FX;
        } // endif

      close(fp->Handle);
#endif   // UNIX
    } else
      // True vector table, Table file size does not change.
      // Just clean the unused part of the file.
      for (n = Fpos - Tpos, i = 0; i < Ncol; i++)
        memset(Memcol[i] + Tpos * Clens[i], 0, n * Clens[i]);

    // Reset Last and Block values in the catalog
    PlugCloseFile(g, To_Fb);      // in case of Header
    ResetTableSize(g, Block, Last);
  } // endif irc

  return RC_OK;                                      // All is correct
  } // end of DeleteRecords

/***********************************************************************/
/*  Move intermediate deleted or updated lines.                        */
/***********************************************************************/
bool VCMFAM::MoveIntermediateLines(PGLOBAL, bool *)
  {
  int i, m, n;

  if ((n = Fpos - Spos) > 0) {
    /*******************************************************************/
    /*  Non consecutive line to delete. Move intermediate lines.       */
    /*******************************************************************/
    if (!MaxBlk) {
      // Old VCT format, moving must respect block limits
      char *ps, *pt;
      int   req, soff, toff;

      for (; n > 0; n -= req) {
        soff = Spos % Nrec;
        toff = Tpos % Nrec;
        req = (size_t)MY_MIN(n, Nrec - MY_MAX(soff, toff));

        for (i = 0; i < Ncol; i++) {
          ps = Memcol[i] + (Spos / Nrec) * Blksize + soff * Clens[i];
          pt = Memcol[i] + (Tpos / Nrec) * Blksize + toff * Clens[i];
          memmove(pt, ps, req * Clens[i]);
          } // endfor i

        Tpos += req;
        Spos += req;
        } // endfor n

    } else {
      // True vector format, all is simple...
      for (i = 0; i < Ncol; i++) {
        m = Clens[i];
        memmove(Memcol[i] + Tpos * m, Memcol[i] + Spos * m, n * m);
        } // endfor i

      Tpos += n;
    } // endif MaxBlk

    if (trace(1))
      htrc("move %d bytes\n", n);

    } // endif n

  return false;
  } // end of MoveIntermediate Lines

/***********************************************************************/
/*  Data Base close routine for VMP access method.                     */
/***********************************************************************/
void VCMFAM::CloseTableFile(PGLOBAL g, bool)
  {
  int  wrc = RC_OK;
  MODE mode = Tdbp->GetMode();

  if (mode == MODE_INSERT) {
    if (!Closing) {
      if (CurNum) {
        // Some more inserted lines remain to be written
        Last = CurNum;
        Block = CurBlk + 1;
        Closing = true;
        wrc = WriteBuffer(g);
      } else {
        Last = Nrec;
        Block = CurBlk;
        wrc = RC_OK;
      } // endif CurNum

    } else
      wrc = RC_FX;                  // Last write was in error

    PlugCloseFile(g, To_Fb);

    if (wrc != RC_FX)
      /*rc =*/ ResetTableSize(g, Block, Last);

  } else if (mode != MODE_DELETE || Abort)
    PlugCloseFile(g, To_Fb);

  } // end of CloseTableFile

/***********************************************************************/
/*  ReadBlock: Read column values from current block.                  */
/***********************************************************************/
bool VCMFAM::ReadBlock(PGLOBAL, PVCTCOL colp)
  {
  char *mempos;
  int   i = colp->Index - 1;
  int  n = Nrec * ((MaxBlk || Split) ? colp->Clen : Lrecl);

  /*********************************************************************/
  /*  Calculate the start position of the column block to read.        */
  /*********************************************************************/
  mempos = Memcol[i] + n * CurBlk;

  if (trace(1))
    htrc("mempos=%p i=%d Nrec=%d Clen=%d CurBlk=%d\n",
          mempos, i, Nrec, colp->Clen, CurBlk);

  if (colp->GetStatus(BUF_MAPPED))
    colp->Blk->SetValPointer(mempos);

  if (trace(1))
    num_read++;

  return false;
  } // end of ReadBlock

/***********************************************************************/
/*  WriteBlock: Write back current column values for one block.        */
/*  Note: there is nothing to do because we are working directly into  */
/*  the mapped file, except when checking for Update but in this case  */
/*  we do not want to write back the modifications either.             */
/***********************************************************************/
bool VCMFAM::WriteBlock(PGLOBAL, PVCTCOL colp __attribute__((unused)))
  {
#if defined(_DEBUG)
  char *mempos;
  int   i = colp->Index - 1;
  int   n = Nrec * colp->Clen;

  /*********************************************************************/
  /*  Calculate the offset and size of the block to write.             */
  /*********************************************************************/
  mempos = Memcol[i] + n * CurBlk;

  if (trace(1))
    htrc("modif=%d mempos=%p i=%d Nrec=%d Clen=%d colblk=%d\n",
          Modif, mempos, i, Nrec, colp->Clen, colp->ColBlk);

#endif // _DEBUG

  return false;
  } // end of WriteBlock

/* -------------------------- Class VECFAM --------------------------- */

/***********************************************************************/
/*  Implementation of the VECFAM class.                                */
/***********************************************************************/
VECFAM::VECFAM(PVCTDEF tdp) : VCTFAM((PVCTDEF)tdp)
  {
  Streams = NULL;
  To_Fbs = NULL;
  To_Bufs = NULL;
  Split = true;
  Block = Last = -1;
  InitUpdate = false;
  } // end of VECFAM standard constructor

VECFAM::VECFAM(PVECFAM txfp) : VCTFAM(txfp)
  {
  Streams = txfp->Streams;
  To_Fbs = txfp->To_Fbs;
  Clens = txfp->Clens;
  To_Bufs = txfp->To_Bufs;
  InitUpdate = txfp->InitUpdate;
  } // end of VECFAM copy constructor

/***********************************************************************/
/*  VEC Access Method opening routine.                                 */
/*  New method now that this routine is called recursively (last table */
/*  first in reverse order): index blocks are immediately linked to    */
/*  join block of next table if it exists or else are discarted.       */
/***********************************************************************/
bool VECFAM::OpenTableFile(PGLOBAL g)
  {
  char    opmode[4];
  int     i;
  bool    b= false;
  PCOLDEF cdp;
  PVCTCOL cp;
  MODE    mode = Tdbp->GetMode();
  PDOSDEF defp = (PDOSDEF)Tdbp->GetDef();

  /*********************************************************************/
  /*  Call Cardinality to set Block and Last values in case it was not */
  /*  already called (this happens indeed in test xmode)               */
  /*********************************************************************/
  Cardinality(g);

  /*********************************************************************/
  /*  Open according to input/output mode required.                    */
  /*********************************************************************/
  switch (mode) {
    case MODE_READ:
      strcpy(opmode, "rb");
      break;
    case MODE_DELETE:
      if (!Tdbp->GetNext()) {
        // Store the number of deleted lines
        DelRows = Cardinality(g);

        // This will delete the whole file
        strcpy(opmode, "wb");

        // This will stop the process by causing GetProgMax to return 0.
        ResetTableSize(g, 0, Nrec);
        break;
        } // endif filter

      // Selective delete, pass thru
      /* fall through */
    case MODE_UPDATE:
      UseTemp = Tdbp->IsUsingTemp(g);
      strcpy(opmode, (UseTemp) ? "rb": "r+b");
      break;
    case MODE_INSERT:
      strcpy(opmode, "ab");
      break;
    default:
      sprintf(g->Message, MSG(BAD_OPEN_MODE), mode);
      return true;
    } // endswitch Mode

  /*********************************************************************/
  /*  Initialize the array of file structures.                         */
  /*********************************************************************/
  if (!Colfn) {
    // Prepare the column file name pattern and set Ncol
    Colfn = (char*)PlugSubAlloc(g, NULL, _MAX_PATH);
    Ncol = ((VCTDEF*)Tdbp->GetDef())->MakeFnPattern(Colfn);
    } // endif Colfn

  Streams = (FILE* *)PlugSubAlloc(g, NULL, Ncol * sizeof(FILE*));
  To_Fbs = (PFBLOCK *)PlugSubAlloc(g, NULL, Ncol * sizeof(PFBLOCK));

  for (i = 0; i < Ncol; i++) {
    Streams[i] = NULL;
    To_Fbs[i] = NULL;
    } // endif i

  /*********************************************************************/
  /*  Open the files corresponding to columns used in the query.       */
  /*********************************************************************/
  if (mode == MODE_INSERT || mode == MODE_DELETE) {
    // All columns must be written or deleted
    for (i = 0, cdp = defp->GetCols(); cdp; i++, cdp = cdp->GetNext())
      if (OpenColumnFile(g, opmode, i))
        return true;

    // Check for void table or missing columns
    for (b = !Streams[0], i = 1; i < Ncol; i++)
      if (b != !Streams[i])
        return true;

  } else {
    /*******************************************************************/
    /*  Open the files corresponding to updated columns of the query.  */
    /*******************************************************************/
    for (cp = (PVCTCOL)((PTDBVCT)Tdbp)->To_SetCols; cp;
         cp = (PVCTCOL)cp->Next)
      if (OpenColumnFile(g, opmode, cp->Index - 1))
        return true;

    // Open in read only mode the used columns not already open
    for (cp = (PVCTCOL)Tdbp->GetColumns(); cp; cp = (PVCTCOL)cp->Next)
      if (!cp->IsSpecial() && !Streams[cp->Index - 1])
        if (OpenColumnFile(g, "rb", cp->Index - 1))
          return true;

    // Check for void table or missing columns
    for (i = 0, cp = (PVCTCOL)Tdbp->GetColumns(); cp;
                cp = (PVCTCOL)cp->Next)
      if (!cp->IsSpecial()) {
        if (!i++)
          b = !Streams[cp->Index - 1];
        else if (b != !Streams[cp->Index - 1])
          return true;
    
        } // endif Special

  } // endif mode

  /*********************************************************************/
  /*  Allocate the table and column block buffer.                      */
  /*********************************************************************/
  return (b) ? false : AllocateBuffer(g);
  } // end of OpenTableFile

/***********************************************************************/
/*  Open the file corresponding to one column.                         */
/***********************************************************************/
bool VECFAM::OpenColumnFile(PGLOBAL g, PCSZ opmode, int i)
  {
  char    filename[_MAX_PATH];
  PDBUSER dup = PlgGetUser(g);

  sprintf(filename, Colfn, i+1);

  if (!(Streams[i] = PlugOpenFile(g, filename, opmode))) {
    if (trace(1))
      htrc("%s\n", g->Message);

    return (Tdbp->GetMode() == MODE_READ && errno == ENOENT)
            ? PushWarning(g, Tdbp) : true;
    } // endif Streams

  if (trace(1))
    htrc("File %s is open in mode %s\n", filename, opmode);

  To_Fbs[i] = dup->Openlist;       // Keep track of File blocks
  return false;
  } // end of OpenColumnFile

/***********************************************************************/
/*  Allocate the block buffers for columns used in the query.          */
/***********************************************************************/
bool VECFAM::AllocateBuffer(PGLOBAL g)
  {
  int     i;
  PVCTCOL cp;
  PCOLDEF cdp;
  PTDBVCT tdbp = (PTDBVCT)Tdbp;
  MODE    mode = tdbp->GetMode();
  PDOSDEF defp = (PDOSDEF)tdbp->GetDef();

  if (mode != MODE_READ) {
    // Allocate what is needed by all modes except Read
    T_Streams = (FILE* *)PlugSubAlloc(g, NULL, Ncol * sizeof(FILE*));
    Clens = (int*)PlugSubAlloc(g, NULL, Ncol * sizeof(int));

    // Give default values
    for (i = 0; i < Ncol; i++) {
      T_Streams[i] = Streams[i];
      Clens[i] = 0;
      } // endfor i

    } // endif mode

  if (mode == MODE_INSERT) {
    bool    chk = PlgGetUser(g)->Check & CHK_TYPE;

    To_Bufs = (void**)PlugSubAlloc(g, NULL, Ncol * sizeof(void*));
    cdp = defp->GetCols();

    for (i = 0; cdp && i < Ncol; i++, cdp = cdp->GetNext()) {
      Clens[i] = cdp->GetClen();
      To_Bufs[i] = PlugSubAlloc(g, NULL, Nrec * Clens[i]);

      if (cdp->GetType() == TYPE_STRING)
        memset(To_Bufs[i], ' ', Nrec * Clens[i]);
      else
        memset(To_Bufs[i],   0, Nrec * Clens[i]);

      } // endfor cdp

    for (cp = (PVCTCOL)tdbp->Columns; cp; cp = (PVCTCOL)cp->Next)
      cp->Blk = AllocValBlock(g, To_Bufs[cp->Index - 1],
                              cp->Buf_Type, Nrec, cp->Format.Length,
                              cp->Format.Prec, chk, true, cp->IsUnsigned());

    return InitInsert(g);
  } else {
    if (UseTemp || mode == MODE_DELETE) {
      // Allocate all that is needed to move lines and make Temp
      if (UseTemp) {
        Tempat = (char*)PlugSubAlloc(g, NULL, _MAX_PATH);
        strcpy(Tempat, Colfn);
        PlugSetPath(Tempat, Tempat, Tdbp->GetPath());
        strcat(PlugRemoveType(Tempat, Tempat), ".t");
        T_Fbs = (PFBLOCK *)PlugSubAlloc(g, NULL, Ncol * sizeof(PFBLOCK));
        } // endif UseTemp

      if (UseTemp)
        for (i = 0; i < Ncol; i++) {
          T_Streams[i] = (mode == MODE_UPDATE) ? (FILE*)1 : NULL;
          T_Fbs[i] = NULL;
          } // endfor i

      if (mode == MODE_DELETE) {  // All columns are moved
        cdp = defp->GetCols();

        for (i = 0; cdp && i < Ncol; i++, cdp = cdp->GetNext()) {
          Clens[i] = cdp->GetClen();
          Buflen = MY_MAX(Buflen, cdp->GetClen());
          } // endfor cdp

      } else {  // Mode Update, only some columns are updated
        for (cp = (PVCTCOL)tdbp->To_SetCols; cp; cp = (PVCTCOL)cp->Next) {
          i = cp->Index -1;

          if (UseTemp)
            T_Streams[i] = NULL;   // Mark the streams to open

          Clens[i] = cp->Clen;
          Buflen = MY_MAX(Buflen, cp->Clen);
          } // endfor cp

        InitUpdate = true;         // To be initialized
      } // endif mode

      To_Buf = (char*)PlugSubAlloc(g, NULL, Buflen * Nrec);
      } // endif mode

    // Finally allocate column buffers for all modes
    for (cp = (PVCTCOL)tdbp->Columns; cp; cp = (PVCTCOL)cp->Next)
      if (!cp->IsSpecial())            // Not a pseudo column
          cp->Blk = AllocValBlock(g, NULL, cp->Buf_Type, Nrec,
                                  cp->Format.Length, cp->Format.Prec,
						                      true, true, cp->IsUnsigned());

  } // endif mode

  return false;
  } // end of AllocateBuffer

/***********************************************************************/
/*  Do initial action when inserting.                                  */
/***********************************************************************/
bool VECFAM::InitInsert(PGLOBAL)
  {
  // We come here in MODE_INSERT only
  CurBlk = 0;
  CurNum = 0;
  AddBlock = true;
  return false;
  } // end of InitInsert

/***********************************************************************/
/*  Reset buffer access according to indexing and to mode.             */
/*  >>>>>>>>>>>>>> TO BE RE-VISITED AND CHECKED <<<<<<<<<<<<<<<<<<<<<< */
/***********************************************************************/
void VECFAM::ResetBuffer(PGLOBAL g)
  {
  /*********************************************************************/
  /*  If access is random, performances can be much better when the    */
  /*  reads are done on only one row, except for small tables that can */
  /*  be entirely read in one block. If the index is just used as a    */
  /*  bitmap filter, as for Update or Delete, reading will be          */
  /*  sequential and we better keep block reading.                     */
  /*********************************************************************/
  if (Tdbp->GetKindex() && Block > 1 && Tdbp->GetMode() == MODE_READ) {
    Nrec = 1;                       // Better for random access
    Rbuf = 0;
    OldBlk = -2;                    // Has no meaning anymore
    Block = Tdbp->Cardinality(g);   // Blocks are one line now
    Last = 1;                       // Probably unuseful
    } // endif Mode

  } // end of ResetBuffer

/***********************************************************************/
/*  Data Base write routine for VCT access method.                     */
/***********************************************************************/
int VECFAM::WriteBuffer(PGLOBAL g)
  {
  if (trace(1))
    htrc("VCT WriteBuffer: R%d Mode=%d CurNum=%d CurBlk=%d\n",
          Tdbp->GetTdb_No(), Tdbp->GetMode(), CurNum, CurBlk);

  if (Tdbp->GetMode() == MODE_INSERT) {
    if (Closing || ++CurNum == Nrec) {
      // Here we must add a new blocks to the files
      int    i;
      size_t n = (size_t)CurNum;

      for (i = 0; i < Ncol; i++)
        if (n != fwrite(To_Bufs[i], (size_t)Clens[i], n, Streams[i])) {
          sprintf(g->Message, MSG(WRITE_STRERROR), To_File, strerror(errno));
          return RC_FX;
          } // endif

      if (!Closing) {
        CurBlk++;
        CurNum = 0;
        } // endif Closing

      } // endif Closing || CurNum

  } else              // Mode Update
    // Writing updates being done in ReadDB we do initialization only.
    if (InitUpdate) {
      if (OpenTempFile(g))
        return RC_FX;

      InitUpdate = false;                 // Done
      } // endif InitUpdate

  return RC_OK;
  } // end of WriteBuffer

/***********************************************************************/
/*  Data Base delete line routine for split vertical access methods.   */
/*  Note: lines are moved directly in the files (ooops...)             */
/*  Using temp file depends on the Check setting, false by default.    */
/***********************************************************************/
int VECFAM::DeleteRecords(PGLOBAL g, int irc)
  {
  if (trace(1))
    htrc("VEC DeleteDB: rc=%d UseTemp=%d Fpos=%d Tpos=%d Spos=%d\n",
          irc, UseTemp, Fpos, Tpos, Spos);

  if (irc != RC_OK) {
    /*******************************************************************/
    /*  EOF: position Fpos at the end-of-file position.                */
    /*******************************************************************/
    Fpos = Cardinality(g);

    if (trace(1))
      htrc("Fpos placed at file end=%d\n", Fpos);

  } else     // Fpos is the Deleted line position
    Fpos = CurBlk * Nrec + CurNum;

  if (Tpos == Spos) {
    // First line to delete
    if (UseTemp) {
      /*****************************************************************/
      /*  Open the temporary files, Spos is at the beginning of file.  */
      /*****************************************************************/
      if (OpenTempFile(g))
        return RC_FX;

    } else
      /*****************************************************************/
      /*  Move of eventual preceding lines is not required here.      */
      /*  Set the future Tpos, and give Spos a value to block copying. */
      /*****************************************************************/
      Spos = Tpos = Fpos;

    } // endif Tpos == Spos

  /*********************************************************************/
  /*  Move any intermediate lines.                                     */
  /*********************************************************************/
  if (MoveIntermediateLines(g))
    return RC_FX;

  if (irc == RC_OK) {
#ifdef _DEBUG
    assert(Spos == Fpos);
#endif
    Spos++;          // New start position is on next line

    if (trace(1))
      htrc("after: Tpos=%d Spos=%d\n", Tpos, Spos);

  } else {
    /*******************************************************************/
    /*  Last call after EOF has been reached.                          */
    /*******************************************************************/
    if (!UseTemp) {
      /*****************************************************************/
      /* Because the chsize functionality is only accessible with a    */
      /* system call we must close the files and reopen them with the  */
      /* open function (_fopen for MS??) this is still to be checked   */
      /* for compatibility with other OS's.                            */
      /*****************************************************************/
      char filename[_MAX_PATH];
      int  h;                            // File handle, return code

      for (int i = 0; i < Ncol; i++) {
        sprintf(filename, Colfn, i + 1);
        /*rc =*/ PlugCloseFile(g, To_Fbs[i]);

        if ((h= global_open(g, MSGID_OPEN_STRERROR, filename, O_WRONLY)) <= 0)
          return RC_FX;

        /***************************************************************/
        /*  Remove extra records.                                      */
        /***************************************************************/
#if defined(UNIX)
        if (ftruncate(h, (off_t)(Tpos * Clens[i]))) {
          sprintf(g->Message, MSG(TRUNCATE_ERROR), strerror(errno));
          close(h);
          return RC_FX;
          } // endif
#else
        if (chsize(h, Tpos * Clens[i])) {
          sprintf(g->Message, MSG(CHSIZE_ERROR), strerror(errno));
          close(h);
          return RC_FX;
          } // endif
#endif

        close(h);

        if (trace(1))
          htrc("done, h=%d irc=%d\n", h, irc);

        } // endfor i

    } else        // UseTemp
      // Ok, now delete old files and rename new temp files
      if (RenameTempFile(g) == RC_FX)
        return RC_FX;

    // Reset these values for TDBVCT::MakeBlockValues
    Block = (Tpos > 0) ? (Tpos + Nrec - 1) / Nrec : 0;
    Last = (Tpos + Nrec - 1) % Nrec + 1;

    if (ResetTableSize(g, Block, Last))
      return RC_FX;

  } // endif irc

  return RC_OK;                                      // All is correct
  } // end of DeleteRecords

/***********************************************************************/
/*  Open temporary files used while updating or deleting.              */
/*  Note: the files not updated have been given a T_Stream value of 1. */
/***********************************************************************/
bool VECFAM::OpenTempFile(PGLOBAL g)
  {
  char tempname[_MAX_PATH];

  for (int i = 0; i < Ncol; i++)
    if (!T_Streams[i]) {
      /*****************************************************************/
      /*  Open the temporary file, Spos is at the beginning of file.   */
      /*****************************************************************/
      sprintf(tempname, Tempat, i+1);

      if (!(T_Streams[i] = PlugOpenFile(g, tempname, "wb"))) {
        if (trace(1))
          htrc("%s\n", g->Message);

        return true;
      } else
        T_Fbs[i] = PlgGetUser(g)->Openlist;

    } else       // This is a column that is not updated
      T_Streams[i] = NULL;        // For RenameTempFile

  return false;
  } // end of OpenTempFile

/***********************************************************************/
/*  Move intermediate updated lines before writing blocks.             */
/***********************************************************************/
bool VECFAM::MoveLines(PGLOBAL g)
  {
  if (UseTemp && !InitUpdate) {    // Don't do it in check pass
    Fpos = OldBlk * Nrec;

    if (MoveIntermediateLines(g)) {
      Closing = true;           // ???
      return true;
      } // endif UseTemp

//  Spos = Fpos + Nrec;
    } // endif UseTemp
  return false;

  } // end of MoveLines

/***********************************************************************/
/*  Move intermediate deleted or updated lines.                        */
/***********************************************************************/
bool VECFAM::MoveIntermediateLines(PGLOBAL g, bool *)
  {
  int    i, n;
  bool   b = false;
  size_t req, len;

  for (n = Fpos - Spos; n > 0; n -= Nrec) {
    /*******************************************************************/
    /*  Non consecutive line to delete. Move intermediate lines.       */
    /*******************************************************************/
    req = (size_t)MY_MIN(n, Nrec);

    for (i = 0; i < Ncol; i++) {
      if (!T_Streams[i])
        continue;             // Non updated column

      if (!UseTemp || !b)
        if (fseek(Streams[i], Spos * Clens[i], SEEK_SET)) {
          sprintf(g->Message, MSG(READ_SEEK_ERROR), strerror(errno));
          return true;
          } // endif

      len = fread(To_Buf, Clens[i], req, Streams[i]);

      if (trace(1))
        htrc("after read req=%d len=%d\n", req, len);

      if (len != req) {
        sprintf(g->Message, MSG(DEL_READ_ERROR), (int) req, (int) len);
        return true;
        } // endif len

      if (!UseTemp)
        if (fseek(T_Streams[i], Tpos * Clens[i], SEEK_SET)) {
          sprintf(g->Message, MSG(WRITE_SEEK_ERR), strerror(errno));
          return true;
          } // endif

      if ((len = fwrite(To_Buf, Clens[i], req, T_Streams[i])) != req) {
        sprintf(g->Message, MSG(DEL_WRITE_ERROR), strerror(errno));
        return true;
        } // endif

      if (trace(1))
        htrc("after write pos=%d\n", ftell(Streams[i]));

      } // endfor i

    Tpos += (int)req;
    Spos += (int)req;

    if (trace(1))
      htrc("loop: Tpos=%d Spos=%d\n", Tpos, Spos);

    b = true;
    } // endfor n

  return false;
  } // end of MoveIntermediate Lines

/***********************************************************************/
/*  Delete the old files and rename the new temporary files.           */
/***********************************************************************/
int VECFAM::RenameTempFile(PGLOBAL g)
  {
  char *tempname, filetemp[_MAX_PATH], filename[_MAX_PATH];
  int   rc = RC_OK;

  // Close all files.
  // This loop is necessary because, in case of join,
  // the table files can have been open several times.
  for (PFBLOCK fb = PlgGetUser(g)->Openlist; fb; fb = fb->Next)
    rc = PlugCloseFile(g, fb);

  for (int i = 0; i < Ncol && rc == RC_OK; i++) {
    if (!T_Fbs[i])
      continue;

    tempname = (char*)T_Fbs[i]->Fname;

    if (!Abort) {
      sprintf(filename, Colfn, i+1);
      PlugSetPath(filename, filename, Tdbp->GetPath());
      strcat(PlugRemoveType(filetemp, filename), ".ttt");
      remove(filetemp);   // May still be there from previous error
  
      if (rename(filename, filetemp)) {    // Save file for security
        snprintf(g->Message, MAX_STR, MSG(RENAME_ERROR),
                filename, filetemp, strerror(errno));
        rc = RC_FX;
      } else if (rename(tempname, filename)) {
        snprintf(g->Message, MAX_STR, MSG(RENAME_ERROR),
                tempname, filename, strerror(errno));
        rc = rename(filetemp, filename);   // Restore saved file
        rc = RC_FX;
      } else if (remove(filetemp)) {
        sprintf(g->Message, MSG(REMOVE_ERROR),
                filetemp, strerror(errno));
        rc = RC_INFO;                      // Acceptable
      } // endif's

    } else
      remove(tempname);

    } // endfor i

  return rc;
  } // end of RenameTempFile

/***********************************************************************/
/*  Data Base close routine for VEC access method.                     */
/***********************************************************************/
void VECFAM::CloseTableFile(PGLOBAL g, bool abort)
  {
  int  rc = 0, wrc = RC_OK;
  MODE mode = Tdbp->GetMode();

  Abort = abort;

  if (mode == MODE_INSERT) {
    if (Closing)
      wrc = RC_FX;                  // Last write was in error
    else
      if (CurNum) {
        // Some more inserted lines remain to be written
        Last += (CurBlk * Nrec + CurNum -1);
        Block += (Last / Nrec);
        Last = Last % Nrec + 1;
        Closing = true;
        wrc = WriteBuffer(g);
      } else {
        Block += CurBlk;
        wrc = RC_OK;
      } // endif CurNum

    if (wrc != RC_FX)
      rc = ResetTableSize(g, Block, Last);
    else
			throw 44;

  } else if (mode == MODE_UPDATE) {
    if (UseTemp && !InitUpdate && !Abort) {
      // Write any intermediate lines to temp file
      Fpos = OldBlk * Nrec;
      Abort = MoveIntermediateLines(g) != RC_OK;
//    Spos = Fpos + Nrec;
      } // endif UseTemp

    // Write back to file any pending modifications
    if (wrc == RC_OK)
      for (PVCTCOL colp = (PVCTCOL)((PTDBVCT)Tdbp)->To_SetCols;
                   colp; colp = (PVCTCOL)colp->Next)
        colp->WriteBlock(g);

    if (wrc == RC_OK && UseTemp && !InitUpdate && !Abort) {
      // Write any intermediate lines to temp file
      Fpos = (Block - 1) * Nrec + Last;
      Abort = MoveIntermediateLines(g) != RC_OK;
      } // endif UseTemp

  } // endif's mode

  if (UseTemp && !InitUpdate) {
    // If they are errors, leave files unchanged
    rc = RenameTempFile(g);

  } else if (Streams)
    for (int i = 0; i < Ncol; i++)
      if (Streams[i]) {
        rc = PlugCloseFile(g, To_Fbs[i]);
        Streams[i] = NULL;
        To_Fbs[i] = NULL;
        } // endif Streams

  if (trace(1))
    htrc("VCT CloseTableFile: closing %s wrc=%d rc=%d\n", To_File, wrc, rc);

  } // end of CloseTableFile

/***********************************************************************/
/*  ReadBlock: Read column values from current block.                  */
/***********************************************************************/
bool VECFAM::ReadBlock(PGLOBAL g, PVCTCOL colp)
  {
  int     i, len;
  size_t  n;

  /*********************************************************************/
  /*  Calculate the offset and size of the block to read.              */
  /*********************************************************************/
  len = Nrec * colp->Clen * CurBlk;
  i = colp->Index - 1;

  if (trace(1))
    htrc("len=%d i=%d Nrec=%d Deplac=%d Lrecl=%d CurBlk=%d\n",
          len, i, Nrec, colp->Deplac, Lrecl, CurBlk);

  if (fseek(Streams[i], len, SEEK_SET)) {
    sprintf(g->Message, MSG(FSEEK_ERROR), strerror(errno));
    return true;
    } // endif

  n = fread(colp->Blk->GetValPointer(), (size_t)colp->Clen,
                                        (size_t)Nrec, Streams[i]);

  if (n != (size_t)Nrec && (CurBlk+1 != Block || n != (size_t)Last)) {
    char fn[_MAX_PATH];

    sprintf(fn, Colfn, colp->Index);
#if defined(_WIN32)
    if (feof(Streams[i]))
#else   // !_WIN32
    if (errno == NO_ERROR)
#endif  // !_WIN32
      sprintf(g->Message, MSG(BAD_READ_NUMBER), (int) n, fn);
    else
      sprintf(g->Message, MSG(READ_ERROR),
              fn, strerror(errno));

    if (trace(1))
      htrc(" Read error: %s\n", g->Message);

    return true;
    } // endif

  if (trace(1))
    num_read++;

  return false;
  } // end of ReadBlock

/***********************************************************************/
/*  WriteBlock: Write back current column values for one block.        */
/*  Note: the test of Status is meant to prevent physical writing of   */
/*  the block during the checking loop in mode Update. It is set to    */
/*  BUF_EMPTY when reopening the table between the two loops.          */
/***********************************************************************/
bool VECFAM::WriteBlock(PGLOBAL g, PVCTCOL colp)
  {
  int   i, len;
  size_t n;

  /*********************************************************************/
  /*  Calculate the offset and size of the block to write.             */
  /*********************************************************************/
  len = Nrec * colp->Clen * colp->ColBlk;
  i = colp->Index - 1;

  if (trace(1))
    htrc("modif=%d len=%d i=%d Nrec=%d Deplac=%d Lrecl=%d colblk=%d\n",
          Modif, len, i, Nrec, colp->Deplac, Lrecl, colp->ColBlk);

  if (Tdbp->GetMode() == MODE_UPDATE && !UseTemp)
    if (fseek(T_Streams[i], len, SEEK_SET)) {
      sprintf(g->Message, MSG(FSEEK_ERROR), strerror(errno));
      return true;
      } // endif

  // Here Nrec was changed to CurNum in mode Insert,
  // this is the true number of records to write,
  // this also avoid writing garbage in the file for true vector tables.
  n = (Tdbp->GetMode() == MODE_INSERT) ? CurNum
    : (colp->ColBlk == Block - 1) ? Last : Nrec;

  if (n != fwrite(colp->Blk->GetValPointer(),
                            (size_t)colp->Clen, n, T_Streams[i])) {
    char fn[_MAX_PATH];

    sprintf(fn, (UseTemp) ? Tempat : Colfn, colp->Index);
    sprintf(g->Message, MSG(WRITE_STRERROR), fn, strerror(errno));

    if (trace(1))
      htrc("Write error: %s\n", strerror(errno));

    return true;
  } else
    Spos = Fpos + n;

#if defined(UNIX)
  fflush(Streams[i]); //NGC
#endif
  return false;
  } // end of WriteBlock

/* -------------------------- Class VMPFAM --------------------------- */

/***********************************************************************/
/*  Implementation of the VMPFAM class.                                */
/***********************************************************************/
VMPFAM::VMPFAM(PVCTDEF tdp) : VCMFAM((PVCTDEF)tdp)
  {
  To_Fbs = NULL;
  Split = true;
  Block = Last = -1;
  } // end of VMPFAM standard constructor

VMPFAM::VMPFAM(PVMPFAM txfp) : VCMFAM(txfp)
  {
  To_Fbs = txfp->To_Fbs;
  } // end of VMPFAM copy constructor

/***********************************************************************/
/*  VCT Access Method opening routine.                                 */
/*  New method now that this routine is called recursively (last table */
/*  first in reverse order): index blocks are immediately linked to    */
/*  join block of next table if it exists or else are discarted.       */
/***********************************************************************/
bool VMPFAM::OpenTableFile(PGLOBAL g)
  {
  int     i;
  bool    b = false;
  MODE    mode = Tdbp->GetMode();
  PCOLDEF cdp;
  PVCTCOL cp;
  PDOSDEF defp = (PDOSDEF)Tdbp->GetDef();

  if (mode == MODE_DELETE && !Tdbp->GetNext()) {
    DelRows = Cardinality(g);

    // This will stop the process by causing GetProgMax to return 0.
    ResetTableSize(g, 0, Nrec);
  } else
    Cardinality(g);        // See comment in VECFAM::OpenTbleFile


  /*********************************************************************/
  /*  Prepare the filename pattern for column files and set Ncol.      */
  /*********************************************************************/
  if (!Colfn) {
    // Prepare the column file name pattern
    Colfn = (char*)PlugSubAlloc(g, NULL, _MAX_PATH);
    Ncol = ((VCTDEF*)Tdbp->GetDef())->MakeFnPattern(Colfn);
    } // endif Colfn

  /*********************************************************************/
  /*  Initialize the array of file structures.                         */
  /*********************************************************************/
  Memcol = (char* *)PlugSubAlloc(g, NULL, Ncol * sizeof(char *));
  To_Fbs = (PFBLOCK *)PlugSubAlloc(g, NULL, Ncol * sizeof(PFBLOCK));

  for (i = 0; i < Ncol; i++) {
    Memcol[i] = NULL;
    To_Fbs[i] = NULL;
    } // endif i

  /*********************************************************************/
  /*  Open the files corresponding to columns used in the query.       */
  /*********************************************************************/
  if (mode == MODE_DELETE) {
    // All columns are used in Delete mode
    for (i = 0, cdp = defp->GetCols(); cdp; i++, cdp = cdp->GetNext())
      if (MapColumnFile(g, mode, i))
        return true;

  } else {
    /*******************************************************************/
    /*  Open the files corresponding to updated columns of the query.  */
    /*******************************************************************/
    for (cp = (PVCTCOL)((PTDBVCT)Tdbp)->To_SetCols; cp;
         cp = (PVCTCOL)cp->Next)
      if (MapColumnFile(g, MODE_UPDATE, cp->Index - 1))
        return true;

    /*******************************************************************/
    /* Open other non already open used columns (except pseudos)      */
    /*******************************************************************/
    for (cp = (PVCTCOL)Tdbp->GetColumns(); cp; cp = (PVCTCOL)cp->Next)
      if (!cp->IsSpecial() && !Memcol[cp->Index - 1])
        if (MapColumnFile(g, MODE_READ, cp->Index - 1))
          return true;

    // Check for void table or missing columns
    for (i = 0, cp = (PVCTCOL)Tdbp->GetColumns(); cp;
                cp = (PVCTCOL)cp->Next)
      if (!cp->IsSpecial()) {
        if (!i++)
          b = !Memcol[cp->Index - 1];
        else if (b != !Memcol[cp->Index - 1])
          return true;

        } // endif Special

  } // endif mode

  /*********************************************************************/
  /*  Allocate the table and column block buffer.                      */
  /*********************************************************************/
  return (b) ? false : AllocateBuffer(g);
  } // end of OpenTableFile

/***********************************************************************/
/*  Open the file corresponding to one column.                         */
/***********************************************************************/
bool VMPFAM::MapColumnFile(PGLOBAL g, MODE mode, int i)
  {
  char    filename[_MAX_PATH];
  size_t  len;
  HANDLE  hFile;
  MEMMAP  mm;
  PFBLOCK fp;
  PDBUSER dup = PlgGetUser(g);

  sprintf(filename, Colfn, i+1);

  /*********************************************************************/
  /*  The whole file will be mapped so we can use it as                */
  /*  if it were entirely read into virtual memory.                    */
  /*  Firstly we check whether this file have been already mapped.     */
  /*********************************************************************/
  if (mode == MODE_READ) {
    for (fp = dup->Openlist; fp; fp = fp->Next)
      if (fp->Type == TYPE_FB_MAP && !stricmp(fp->Fname, filename)
                     && fp->Count && fp->Mode == mode)
        break;

    if (trace(1))
      htrc("Mapping file, fp=%p\n", fp);

  } else
    fp = NULL;

  if (fp) {
    /*******************************************************************/
    /*  File already mapped. Just increment use count and get pointer. */
    /*******************************************************************/
    fp->Count++;
    Memcol[i] = fp->Memory;
    len = fp->Length;
  } else {
    /*******************************************************************/
    /*  Create the mapping file object.                                */
    /*******************************************************************/
    hFile = CreateFileMap(g, filename, &mm, mode, DelRows);

    if (hFile == INVALID_HANDLE_VALUE) {
      DWORD rc = GetLastError();

      if (!(*g->Message))
        sprintf(g->Message, MSG(OPEN_MODE_ERROR),
                "map", (int) rc, filename);
      if (trace(1))
        htrc("%s\n", g->Message);

      return (mode == MODE_READ && rc == ENOENT)
              ? PushWarning(g, Tdbp) : true;
      } // endif hFile

    /*****************************************************************/
    /*  Get the file size (assuming file is smaller than 4 GB)       */
    /*****************************************************************/
		len = (size_t)mm.lenL;

		if (mm.lenH)
			len += ((size_t)mm.lenH * 0x000000001LL);

		Memcol[i] = (char *)mm.memory;

    if (!len) {             // Empty or deleted file
      CloseFileHandle(hFile);
      ResetTableSize(g, 0, Nrec);
      return false;
      } // endif len

    if (!Memcol[i]) {
      CloseFileHandle(hFile);
      sprintf(g->Message, MSG(MAP_VIEW_ERROR),
                          filename, GetLastError());
      return true;
      } // endif Memory

    if (mode != MODE_DELETE) {
      CloseFileHandle(hFile);                    // Not used anymore
      hFile = INVALID_HANDLE_VALUE;              // For Fblock
      } // endif Mode

    /*******************************************************************/
    /*  Link a Fblock. This make possible to reuse already opened maps */
    /*  and also to automatically unmap them in case of error g->jump. */
    /*  Note: block can already exist for previously closed file.      */
    /*******************************************************************/
    fp = (PFBLOCK)PlugSubAlloc(g, NULL, sizeof(FBLOCK));
    fp->Type = TYPE_FB_MAP;
    fp->Fname = PlugDup(g, filename);
    fp->Next = dup->Openlist;
    dup->Openlist = fp;
    fp->Count = 1;
    fp->Length = len;
    fp->Memory = Memcol[i];
    fp->Mode = mode;
    fp->File = NULL;
    fp->Handle = hFile;                        // Used for Delete
  } // endif fp

  To_Fbs[i] = fp;                              // Useful when closing

  if (trace(1))
    htrc("fp=%p count=%d MapView=%p len=%d\n",
          fp, fp->Count, Memcol[i], len);

  return false;
  } // end of MapColumnFile

/***********************************************************************/
/*  Allocate the block buffers for columns used in the query.          */
/*  Give a dummy value (1) to prevent allocating the value block.     */
/*  It will be set pointing into the memory map of the file.           */
/***********************************************************************/
bool VMPFAM::AllocateBuffer(PGLOBAL g)
  {
  PVCTCOL cp;

  if (Tdbp->GetMode() == MODE_DELETE) {
    PCOLDEF cdp = Tdbp->GetDef()->GetCols();

    Clens = (int*)PlugSubAlloc(g, NULL, Ncol * sizeof(int));

    for (int i = 0; cdp && i < Ncol; i++, cdp = cdp->GetNext())
      Clens[i] = cdp->GetClen();

    } // endif mode

  for (cp = (PVCTCOL)Tdbp->GetColumns(); cp; cp = (PVCTCOL)cp->Next)
    if (!cp->IsSpecial()) {            // Not a pseudo column
      cp->Blk = AllocValBlock(g, (void*)1, cp->Buf_Type, Nrec,
                              cp->Format.Length, cp->Format.Prec,
				                      true, true, cp->IsUnsigned());
      cp->AddStatus(BUF_MAPPED);
      } // endif IsSpecial

  return false;
  } // end of AllocateBuffer

/***********************************************************************/
/*  Data Base delete line routine for VMP access method.               */
/*  Lines between deleted lines are moved in the mapfile view.         */
/***********************************************************************/
int VMPFAM::DeleteRecords(PGLOBAL g, int irc)
  {
  int  i;
  int m, n;

  if (trace(1))
    htrc("VMP DeleteDB: irc=%d tobuf=%p Tpos=%p Spos=%p\n",
                        irc, To_Buf, Tpos, Spos);

  if (irc != RC_OK) {
    /*******************************************************************/
    /*  EOF: position Fpos at the top of map position.                 */
    /*******************************************************************/
    Fpos = (Block - 1) * Nrec + Last;

    if (trace(1))
      htrc("Fpos placed at file top=%p\n", Fpos);

  } else     // Fpos is the Deleted line position
    Fpos = CurBlk * Nrec + CurNum;

  if (Tpos == Spos) {
    /*******************************************************************/
    /*  First line to delete. Move of eventual preceding lines is     */
    /*  not required here, just setting of future Spos and Tpos.       */
    /*******************************************************************/
    Tpos = Fpos;                               // Spos is set below
  } else if ((n = Fpos - Spos) > 0) {
    /*******************************************************************/
    /*  Non consecutive line to delete. Move intermediate lines.       */
    /*******************************************************************/
    for (i = 0; i < Ncol; i++) {
      m = Clens[i];
      memmove(Memcol[i] + Tpos * m, Memcol[i] + Spos * m, m * n);
      } // endif i

    Tpos += n;

    if (trace(1))
      htrc("move %d bytes\n", n);

    } // endif n

  if (irc == RC_OK) {
    Spos = Fpos + 1;                               // New start position

    if (trace(1))
      htrc("after: Tpos=%p Spos=%p\n", Tpos, Spos);

  } else {
    /*******************************************************************/
    /*  Last call after EOF has been reached.                          */
    /*  We must firstly Unmap the view and use the saved file handle   */
    /*  to put an EOF at the end of the copied part of the file.       */
    /*******************************************************************/
    PFBLOCK fp;

    /*******************************************************************/
    /*  Reset the Block and Last values for TDBVCT::MakeBlockValues.   */
    /*******************************************************************/
//  Block = (Tpos > 0) ? (Tpos + Nrec - 1) / Nrec : 0;
//  Last = (Tpos + Nrec - 1) % Nrec + 1;

    for (i = 0; i < Ncol; i++) {
      fp = To_Fbs[i];
      CloseMemMap(fp->Memory, (size_t)fp->Length);
      fp->Count = 0;                             // Avoid doing it twice

      /*****************************************************************/
      /*  Remove extra records.                                        */
      /*****************************************************************/
      n = Tpos * Clens[i];

#if defined(_WIN32)
      DWORD drc = SetFilePointer(fp->Handle, n, NULL, FILE_BEGIN);

      if (drc == 0xFFFFFFFF) {
        sprintf(g->Message, MSG(FUNCTION_ERROR),
                            "SetFilePointer", GetLastError());
        CloseHandle(fp->Handle);
        return RC_FX;
        } // endif

      if (trace(1))
        htrc("done, Tpos=%p newsize=%d drc=%d\n", Tpos, n, drc);

      if (!SetEndOfFile(fp->Handle)) {
        sprintf(g->Message, MSG(FUNCTION_ERROR),
                            "SetEndOfFile", GetLastError());
        CloseHandle(fp->Handle);
        return RC_FX;
        } // endif

      CloseHandle(fp->Handle);
#else    // UNIX
      if (ftruncate(fp->Handle, (off_t)n)) {
        sprintf(g->Message, MSG(TRUNCATE_ERROR), strerror(errno));
        close(fp->Handle);
        return RC_FX;
        } // endif

      close(fp->Handle);
#endif   // UNIX
      } // endfor i

  } // endif irc

  return RC_OK;                                      // All is correct
  } // end of DeleteRecords

/***********************************************************************/
/*  Data Base close routine for VMP access method.                     */
/***********************************************************************/
void VMPFAM::CloseTableFile(PGLOBAL g, bool)
  {
  if (Tdbp->GetMode() == MODE_DELETE) {
    // Set Block and Nrec values for TDBVCT::MakeBlockValues
    Block = (Tpos > 0) ? (Tpos + Nrec - 1) / Nrec : 0;
    Last = (Tpos + Nrec - 1) % Nrec + 1;
    ResetTableSize(g, Block, Last);
  } else if (Tdbp->GetMode() == MODE_INSERT)
    assert(false);

  for (int i = 0; i < Ncol; i++)
    PlugCloseFile(g, To_Fbs[i]);

  } // end of CloseTableFile

/* -------------------------- Class BGVFAM --------------------------- */

/***********************************************************************/
/*  Implementation of the BGVFAM class.                                */
/***********************************************************************/
// Constructors
BGVFAM::BGVFAM(PVCTDEF tdp) : VCTFAM(tdp)
  {
  Hfile = INVALID_HANDLE_VALUE;
  Tfile = INVALID_HANDLE_VALUE;
  BigDep = NULL;
  } // end of BGVFAM constructor

BGVFAM::BGVFAM(PBGVFAM txfp) : VCTFAM(txfp)
  {
  Hfile = txfp->Hfile;
  Tfile = txfp->Tfile;
  BigDep= txfp->BigDep;
  } // end of BGVFAM copy constructor

/***********************************************************************/
/*  Set current position in a big file.                                */
/***********************************************************************/
bool BGVFAM::BigSeek(PGLOBAL g, HANDLE h, BIGINT pos, bool b)
  {
#if defined(_WIN32)
  char          buf[256];
  DWORD         drc, m = (b) ? FILE_END : FILE_BEGIN;
  LARGE_INTEGER of;

  of.QuadPart = pos;
  of.LowPart = SetFilePointer(h, of.LowPart, &of.HighPart, m);

  if (of.LowPart == INVALID_SET_FILE_POINTER &&
           (drc = GetLastError()) != NO_ERROR) {
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS, NULL, drc, 0,
                  (LPTSTR)buf, sizeof(buf), NULL);
    sprintf(g->Message, MSG(SFP_ERROR), buf);
    return true;
    } // endif
#else   // !_WIN32
  if (lseek64(h, pos, (b) ? SEEK_END : SEEK_SET) < 0) {
    sprintf(g->Message, MSG(ERROR_IN_LSK), errno);
    return true;
    } // endif
#endif  // !_WIN32

  return false;
  } // end of BigSeek

/***********************************************************************/
/*  Read from a big file.                                              */
/***********************************************************************/
bool BGVFAM::BigRead(PGLOBAL g, HANDLE h, void *inbuf, int req)
  {
  bool rc = false;

#if defined(_WIN32)
  DWORD nbr, drc, len = (DWORD)req;
  bool  brc = ReadFile(h, inbuf, len, &nbr, NULL);

  if (trace(1))
    htrc("after read req=%d brc=%d nbr=%d\n", req, brc, nbr);

  if (!brc || nbr != len) {
    char buf[256];  // , *fn = (h == Hfile) ? To_File : "Tempfile";

    if (brc)
      strcpy(buf, MSG(BAD_BYTE_READ));
    else {
      drc = GetLastError();
      FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                    FORMAT_MESSAGE_IGNORE_INSERTS, NULL, drc, 0,
                    (LPTSTR)buf, sizeof(buf), NULL);
      } // endelse brc

    sprintf(g->Message, MSG(READ_ERROR), To_File, buf);

    if (trace(1))
      htrc("BIGREAD: %s\n", g->Message);

    rc = true;
    } // endif brc || nbr
#else   // !_WIN32
  size_t  len = (size_t)req;
  ssize_t nbr = read(h, inbuf, len);

  if (nbr != (ssize_t)len) {
    const char *fn = (h == Hfile) ? To_File : "Tempfile";

    sprintf(g->Message, MSG(READ_ERROR), fn, strerror(errno));

    if (trace(1))
      htrc("BIGREAD: nbr=%d len=%d errno=%d %s\n",
                     nbr, len, errno, g->Message);

    rc = true;
    } // endif nbr
#endif  // !_WIN32

  return rc;
  } // end of BigRead

/***********************************************************************/
/*  Write into a big file.                                             */
/***********************************************************************/
bool BGVFAM::BigWrite(PGLOBAL g, HANDLE h, void *inbuf, int req)
  {
  bool rc = false;

#if defined(_WIN32)
  DWORD nbw, drc, len = (DWORD)req;
  bool  brc = WriteFile(h, inbuf, len, &nbw, NULL);

  if (trace(1))
    htrc("after write req=%d brc=%d nbw=%d\n", req, brc, nbw);

  if (!brc || nbw != len) {
		char buf[256];
		PCSZ fn = (h == Hfile) ? To_File : "Tempfile";

    if (brc)
      strcpy(buf, MSG(BAD_BYTE_NUM));
    else {
      drc = GetLastError();
      FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                    FORMAT_MESSAGE_IGNORE_INSERTS, NULL, drc, 0,
                    (LPTSTR)buf, sizeof(buf), NULL);
      } // endelse brc

    sprintf(g->Message, MSG(WRITE_STRERROR), fn, buf);

    if (trace(1))
      htrc("BIGWRITE: nbw=%d len=%d errno=%d %s\n",
                      nbw, len, drc, g->Message);

    rc = true;
    } // endif brc || nbw
#else   // !_WIN32
  size_t  len = (size_t)req;
  ssize_t nbw = write(h, inbuf, len);

  if (nbw != (ssize_t)len) {
    const char *fn = (h == Hfile) ? To_File : "Tempfile";

    sprintf(g->Message, MSG(WRITE_STRERROR), fn, strerror(errno));

    if (trace(1))
      htrc("BIGWRITE: nbw=%d len=%d errno=%d %s\n",
                      nbw, len, errno, g->Message);

    rc = true;
    } // endif nbr
#endif  // !_WIN32

  return rc;
  } // end of BigWrite

/***********************************************************************/
/*  Get the Headlen, Block and Last info from the file header.         */
/***********************************************************************/
int BGVFAM::GetBlockInfo(PGLOBAL g)
  {
  char      filename[_MAX_PATH];
  int       n;
  VECHEADER vh;
  HANDLE    h;

  if (Header < 1 || Header > 3 || !MaxBlk) {
    sprintf(g->Message, "Invalid header value %d", Header);
    return -1;
  } else
    n = (Header == 1) ? (int)sizeof(VECHEADER) : 0;

  PlugSetPath(filename, To_File, Tdbp->GetPath());

  if (Header == 2)
    strcat(PlugRemoveType(filename, filename), ".blk");

#if defined(_WIN32)
  LARGE_INTEGER len;

  h = CreateFile(filename, GENERIC_READ, FILE_SHARE_READ, NULL,
                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

  if (h != INVALID_HANDLE_VALUE) {
    // Get the size of the file (can be greater than 4 GB)
    len.LowPart = GetFileSize(h, (LPDWORD)&len.HighPart);
    } // endif h

  if (h == INVALID_HANDLE_VALUE || !len.QuadPart) {
#else   // !_WIN32
  h = open64(filename, O_RDONLY, 0);

  if (h == INVALID_HANDLE_VALUE || !_filelength(h)) {
#endif  // !_WIN32
    // Consider this is a void table
    if (trace(1))
      htrc("Void table h=%d\n", h);

    Last = Nrec;
    Block = 0;

    if (h != INVALID_HANDLE_VALUE)
      CloseFileHandle(h);

    return n;
  } else if (Header == 3)
    /*b = */ BigSeek(g, h, -(BIGINT)sizeof(vh), true);

  if (BigRead(g, h, &vh, sizeof(vh))) {
    sprintf(g->Message, "Error reading header file %s", filename);
    n = -1;
  } else if (MaxBlk * Nrec != vh.MaxRec) {
    sprintf(g->Message, "MaxRec=%d doesn't match MaxBlk=%d Nrec=%d",
                        vh.MaxRec, MaxBlk, Nrec);
    n = -1;
  } else {
    Block = (vh.NumRec > 0) ? (vh.NumRec + Nrec - 1) / Nrec : 0;
    Last  = (vh.NumRec + Nrec - 1) % Nrec + 1;

    if (trace(1))
      htrc("Block=%d Last=%d\n", Block, Last);

  } // endif's

  CloseFileHandle(h);
  return n;
  } // end of GetBlockInfo

/***********************************************************************/
/*  Set the MaxRec and NumRec info in the file header.                 */
/***********************************************************************/
bool BGVFAM::SetBlockInfo(PGLOBAL g)
  {
  char      filename[_MAX_PATH];
  bool      b = false, rc = false;
  VECHEADER vh;
  HANDLE    h = INVALID_HANDLE_VALUE;

  PlugSetPath(filename, To_File, Tdbp->GetPath());

  if (Header != 2) {
    if (Hfile != INVALID_HANDLE_VALUE) {
      h = Hfile;

      if (Header == 1)
        /*bk =*/ BigSeek(g, h, (BIGINT)0);

    } else
      b = true;

  } else       // Header == 2
    strcat(PlugRemoveType(filename, filename), ".blk");

  if (h == INVALID_HANDLE_VALUE) {
#if defined(_WIN32)
    DWORD creation = (b) ? OPEN_EXISTING : TRUNCATE_EXISTING;

    h = CreateFile(filename, GENERIC_READ | GENERIC_WRITE, 0,
                   NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL);

#else   // !_WIN32
    int oflag = (b) ?  O_RDWR : O_RDWR | O_TRUNC;

    h = open64(filename, oflag, 0);
#endif  // !_WIN32

    if (h == INVALID_HANDLE_VALUE) {
      sprintf(g->Message, "Error opening header file %s", filename);
      return true;
      } // endif h

    } // endif h

  if (Header == 3)
    /*bk =*/ BigSeek(g, h, -(BIGINT)sizeof(vh), true);

  vh.MaxRec = MaxBlk * Bsize;
  vh.NumRec = (Block - 1) * Nrec + Last;

  if (BigWrite(g, h, &vh, sizeof(vh))) {
    sprintf(g->Message, "Error writing header file %s", filename);
    rc = true;
    } // endif fread

  if (Header == 2 || Hfile == INVALID_HANDLE_VALUE)
    CloseFileHandle(h);

  return rc;
  } // end of SetBlockInfo

/***********************************************************************/
/*  VEC Create an empty file for new Vector formatted tables.          */
/***********************************************************************/
bool BGVFAM::MakeEmptyFile(PGLOBAL g, PCSZ fn)
  {
  // Vector formatted file this will create an empty file of the
  // required length if it does not exists yet.
  char          filename[_MAX_PATH], c = 0;
  int           n = (Header == 1 || Header == 3) ? sizeof(VECHEADER) : 0;

  PlugSetPath(filename, fn, Tdbp->GetPath());

#if defined(_WIN32)
  PCSZ          p;
  DWORD         rc;
  bool          brc;
  LARGE_INTEGER of;
  HANDLE        h;

  h = CreateFile(filename, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);

  if (h == INVALID_HANDLE_VALUE) {
    p = MSG(OPENING);
    goto err;
    } // endif h

  of.QuadPart = (BIGINT)n + (BIGINT)MaxBlk * (BIGINT)Blksize - (BIGINT)1;

  if (trace(1))
    htrc("MEF: of=%lld n=%d maxblk=%d blksize=%d\n",
               of.QuadPart, n, MaxBlk, Blksize);

  of.LowPart = SetFilePointer(h, of.LowPart,
                                &of.HighPart, FILE_BEGIN);

  if (of.LowPart == INVALID_SET_FILE_POINTER &&
           GetLastError() != NO_ERROR) {
    p = MSG(MAKING);
    goto err;
    } // endif

  brc = WriteFile(h, &c, 1, &rc, NULL);

  if (!brc || rc != 1) {
    p = MSG(WRITING);
    goto err;
    } // endif

  CloseHandle(h);
  return false;

 err:
  rc = GetLastError();
  sprintf(g->Message, MSG(EMPTY_FILE), p, filename);
  FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS, NULL, rc, 0,
                (LPTSTR)filename, sizeof(filename), NULL);
  strcat(g->Message, filename);

  if (h != INVALID_HANDLE_VALUE)
    CloseHandle(h);

  return true;
#else   // !_WIN32
  int    h;
  BIGINT pos;

  h= open64(filename, O_CREAT | O_WRONLY, S_IREAD | S_IWRITE);

  if (h == -1)
    return true;

  pos = (BIGINT)n + (BIGINT)MaxBlk * (BIGINT)Blksize - (BIGINT)1;

  if (trace(1))
    htrc("MEF: pos=%lld n=%d maxblk=%d blksize=%d\n",
               pos, n, MaxBlk, Blksize);

  if (lseek64(h, pos, SEEK_SET) < 0)
    goto err;

  // This actually fills the empty file
  if (write(h, &c, 1) < 0)
    goto err;
       
  close(h);
  return false;
  
 err:
  sprintf(g->Message, MSG(MAKE_EMPTY_FILE), To_File, strerror(errno));
  close(h);
  return true;
#endif  // !_WIN32
  } // end of MakeEmptyFile

/***********************************************************************/
/*  Vopen function: opens a file using Windows or Unix API's.          */
/***********************************************************************/
bool BGVFAM::OpenTableFile(PGLOBAL g)
  {
  char    filename[_MAX_PATH];
  bool    del = false;
  MODE    mode = Tdbp->GetMode();
  PDBUSER dbuserp = PlgGetUser(g);

  if ((To_Fb && To_Fb->Count) || Hfile != INVALID_HANDLE_VALUE) {
    sprintf(g->Message, MSG(FILE_OPEN_YET), To_File);
    return true;
    } // endif

  /*********************************************************************/
  /*  Update block info if necessary.                                  */
  /*********************************************************************/
  if (Block < 0)
    if ((Headlen = GetBlockInfo(g)) < 0)
      return true;

  PlugSetPath(filename, To_File, Tdbp->GetPath());

  if (trace(1))
    htrc("OpenTableFile: filename=%s mode=%d Last=%d\n",
                         filename, mode, Last);

#if defined(_WIN32)
  DWORD access, creation, share = 0, rc = 0;

  /*********************************************************************/
  /*  Create the file object according to access mode                  */
  /*********************************************************************/
  switch (mode) {
    case MODE_READ:
      access = GENERIC_READ;
      share = FILE_SHARE_READ;
      creation = OPEN_EXISTING;
      break;
    case MODE_INSERT:
      if (MaxBlk) {
        if (!Block)
          if (MakeEmptyFile(g, To_File))
            return true;

        // Required to update empty blocks
        access = GENERIC_READ | GENERIC_WRITE;
      } else if (Last == Nrec)
        access = GENERIC_WRITE;
      else
        // Required to update the last block
        access = GENERIC_READ | GENERIC_WRITE;

      creation = OPEN_ALWAYS;
      break;
    case MODE_DELETE:
      if (!Tdbp->GetNext()) {
        // Store the number of deleted lines
        DelRows = Cardinality(g);

        // This will stop the process by
        // causing GetProgMax to return 0.
//      ResetTableSize(g, 0, Nrec);     must be done later
        del = true;

        // This will delete the whole file
        access = GENERIC_READ | GENERIC_WRITE;
        creation = TRUNCATE_EXISTING;
        break;
        } // endif

      // Selective delete, pass thru
    case MODE_UPDATE:
      if ((UseTemp = Tdbp->IsUsingTemp(g)))
        access = GENERIC_READ;
      else
        access = GENERIC_READ | GENERIC_WRITE;

      creation = OPEN_EXISTING;
      break;
    default:
      sprintf(g->Message, MSG(BAD_OPEN_MODE), mode);
      return true;
    } // endswitch

  /*********************************************************************/
  /*  Use specific Windows API functions.                              */
  /*********************************************************************/
  Hfile = CreateFile(filename, access, share, NULL, creation,
                               FILE_ATTRIBUTE_NORMAL, NULL);

  if (Hfile == INVALID_HANDLE_VALUE) {
    rc = GetLastError();
    sprintf(g->Message, MSG(OPEN_ERROR), rc, mode, filename);
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS, NULL, rc, 0,
                  (LPTSTR)filename, sizeof(filename), NULL);
    strcat(g->Message, filename);
    } // endif Hfile

  if (trace(1))
    htrc(" rc=%d access=%p share=%p creation=%d handle=%p fn=%s\n",
           rc, access, share, creation, Hfile, filename);

  if (mode == MODE_INSERT) {
    /*******************************************************************/
    /* In Insert mode we must position the cursor at end of file.      */
    /*******************************************************************/
    LARGE_INTEGER of;

    of.QuadPart = (BIGINT)0;
    of.LowPart = SetFilePointer(Hfile, of.LowPart,
                                      &of.HighPart, FILE_END);

    if (of.LowPart == INVALID_SET_FILE_POINTER &&
       (rc = GetLastError()) != NO_ERROR) {
      sprintf(g->Message, MSG(ERROR_IN_SFP), rc);
      CloseHandle(Hfile);
      Hfile = INVALID_HANDLE_VALUE;
      } // endif

    } // endif Mode

#else   // UNIX
  /*********************************************************************/
  /*  The open() function has a transitional interface for 64-bit      */
  /*  file  offsets. Note that using open64() is equivalent to using   */
  /*  open() with O_LARGEFILE set in oflag (see Xopen in tabfix.cpp).  */
  /*********************************************************************/
  int    rc = 0;
  int    oflag;
  mode_t pmd = 0;

  /*********************************************************************/
  /*  Create the file object according to access mode                  */
  /*********************************************************************/
  switch (mode) {
    case MODE_READ:
      oflag = O_RDONLY;
      break;
    case MODE_INSERT:
      if (MaxBlk) {
        if (!Block)
          if (MakeEmptyFile(g, To_File))
            return true;

        // Required to update empty blocks
        oflag = O_RDWR;
      } else if (Last == Nrec)
        oflag = O_WRONLY | O_CREAT | O_APPEND;
      else
        // Required to update the last block
        oflag = O_RDWR | O_CREAT | O_APPEND;

      pmd = S_IREAD | S_IWRITE;
      break;
    case MODE_DELETE:
      // This is temporary until a partial delete is implemented
      if (!Tdbp->GetNext()) {
        // Store the number of deleted lines
        DelRows = Cardinality(g);
        del = true;

        // This will delete the whole file and provoque ReadDB to
        // return immediately.
        oflag = O_RDWR | O_TRUNC;
        strcpy(g->Message, MSG(NO_VCT_DELETE));
        break;
        } // endif

      // Selective delete, pass thru
      /* fall through */
    case MODE_UPDATE:
      UseTemp = Tdbp->IsUsingTemp(g);
      oflag = (UseTemp) ? O_RDONLY : O_RDWR;
      break;
    default:
      sprintf(g->Message, MSG(BAD_OPEN_MODE), mode);
      return true;
    } // endswitch

  Hfile = open64(filename, oflag, pmd); // Enable file size > 2G

  if (Hfile == INVALID_HANDLE_VALUE) {
    rc = errno;
    sprintf(g->Message, MSG(OPEN_ERROR), rc, mode, filename);
    strcat(g->Message, strerror(errno));
    } // endif Hfile

  if (trace(1))
    htrc(" rc=%d oflag=%p mode=%p handle=%d fn=%s\n",
           rc, oflag, mode, Hfile, filename);
#endif  // UNIX

  if (!rc) {
    if (!To_Fb) {
      To_Fb = (PFBLOCK)PlugSubAlloc(g, NULL, sizeof(FBLOCK));
      To_Fb->Fname = To_File;
      To_Fb->Type = TYPE_FB_HANDLE;
      To_Fb->Memory = NULL;
      To_Fb->Length = 0;
      To_Fb->File = NULL;
      To_Fb->Next = dbuserp->Openlist;
      dbuserp->Openlist = To_Fb;
      } // endif To_Fb

    To_Fb->Count = 1;
    To_Fb->Mode = mode;
    To_Fb->Handle = Hfile;

    if (trace(1))
      htrc("File %s is open in mode %d\n", filename, mode);

    if (del)
      // This will stop the process by
      // causing GetProgMax to return 0.
      return ResetTableSize(g, 0, Nrec);

    /*********************************************************************/
    /*  Allocate the table and column block buffers.                     */
    /*********************************************************************/
    return AllocateBuffer(g);
  } else
    return (mode == MODE_READ && rc == ENOENT)
            ? PushWarning(g, Tdbp) : true;

  } // end of OpenTableFile

/***********************************************************************/
/*  Allocate the block buffers for columns used in the query.          */
/***********************************************************************/
bool BGVFAM::AllocateBuffer(PGLOBAL g)
  {
  MODE    mode = Tdbp->GetMode();
  PDOSDEF defp = (PDOSDEF)Tdbp->GetDef();
  PCOLDEF cdp;
  PVCTCOL cp = (PVCTCOL)Tdbp->GetColumns();

  if (mode == MODE_INSERT) {
    if (!NewBlock) {
      // Not reopening after inserting the last block
      bool chk = PlgGetUser(g)->Check & CHK_TYPE;

      NewBlock = (char*)PlugSubAlloc(g, NULL, Blksize);

      for (cdp = defp->GetCols(); cdp; cdp = cdp->GetNext())
        memset(NewBlock + Nrec * cdp->GetPoff(),
              (IsTypeNum(cdp->GetType()) ? 0 : ' '),
                          Nrec * cdp->GetClen());

      for (; cp; cp = (PVCTCOL)cp->Next)
        cp->Blk = AllocValBlock(g, NewBlock + Nrec * cp->Deplac,
                                cp->Buf_Type, Nrec, cp->Format.Length,
                                cp->Format.Prec, chk, true, cp->IsUnsigned());

      InitInsert(g);    // Initialize inserting

      // Currently we don't use a temporary file for inserting
      Tfile = Hfile;
      } // endif NewBlock

  } else {
    if (UseTemp || mode == MODE_DELETE) {
      // Allocate all that is needed to move lines
      int i = 0;

      if (!Ncol)
        for (cdp = defp->GetCols(); cdp; cdp = cdp->GetNext())
          Ncol++;

      if (MaxBlk)
        BigDep = (BIGINT*)PlugSubAlloc(g, NULL, Ncol * sizeof(BIGINT));
      else
        Deplac = (int*)PlugSubAlloc(g, NULL, Ncol * sizeof(int));

      Clens = (int*)PlugSubAlloc(g, NULL, Ncol * sizeof(int));
      Isnum = (bool*)PlugSubAlloc(g, NULL, Ncol * sizeof(bool));

      for (cdp = defp->GetCols(); cdp; i++, cdp = cdp->GetNext()) {
        if (MaxBlk)
          BigDep[i] = (BIGINT)Headlen
                    + (BIGINT)(cdp->GetPoff() * Nrec) * (BIGINT)MaxBlk;
        else
          Deplac[i] = cdp->GetPoff() * Nrec;

        Clens[i] = cdp->GetClen();
        Isnum[i] = IsTypeNum(cdp->GetType());
        Buflen = MY_MAX(Buflen, cdp->GetClen());
        } // endfor cdp

      if (!UseTemp || MaxBlk) {
        Buflen *= Nrec;
        To_Buf = (char*)PlugSubAlloc(g, NULL, Buflen);
      } else
        NewBlock = (char*)PlugSubAlloc(g, NULL, Blksize);

      } // endif mode

    for (; cp; cp = (PVCTCOL)cp->Next)
      if (!cp->IsSpecial())            // Not a pseudo column
        cp->Blk = AllocValBlock(g, NULL, cp->Buf_Type, Nrec,
                                cp->Format.Length, cp->Format.Prec,
					                      true, true, cp->IsUnsigned());

  } //endif mode

  return false;
  } // end of AllocateBuffer

/***********************************************************************/
/*  Data Base write routine for huge VCT access method.                */
/***********************************************************************/
int BGVFAM::WriteBuffer(PGLOBAL g)
  {
  if (trace(1))
    htrc("BGV WriteDB: R%d Mode=%d CurNum=%d CurBlk=%d\n",
          Tdbp->GetTdb_No(), Tdbp->GetMode(), CurNum, CurBlk);

  if (Tdbp->GetMode() == MODE_UPDATE) {
    // Mode Update is done in ReadDB, we just initialize it here
    if (Tfile == INVALID_HANDLE_VALUE) {
      if (UseTemp) {
        if (OpenTempFile(g))
          return RC_FX;

        // Most of the time, not all table columns are updated.
        // This why we must completely pre-fill the temporary file.
        Fpos = (MaxBlk) ? (Block - 1) * Nrec + Last
                        : Block * Nrec;   // To write last lock

        if (MoveIntermediateLines(g))
          return RC_FX;

      } else
        Tfile = Hfile;

      } // endif Tfile

  } else {
    // Mode Insert
    if (MaxBlk && CurBlk == MaxBlk) {
      strcpy(g->Message, MSG(TRUNC_BY_ESTIM));
      return RC_EF;       // Too many lines for a Vector formatted table
      } // endif MaxBlk

    if (Closing || ++CurNum == Nrec) {
      PVCTCOL cp = (PVCTCOL)Tdbp->GetColumns();

      if (!AddBlock) {
        // Write back the updated last block values
        for (; cp; cp = (PVCTCOL)cp->Next)
          cp->WriteBlock(g);

        if (!Closing && !MaxBlk) {
          // Close the VCT file and reopen it in mode Insert
//#if defined(_WIN32)  //OB
//          CloseHandle(Hfile);
//#else    // UNIX
//          close(Hfile);
//#endif   // UNIX
          CloseFileHandle(Hfile);
          Hfile = INVALID_HANDLE_VALUE;
          To_Fb->Count = 0;
          Last = Nrec;               // Tested in OpenTableFile

          if (OpenTableFile(g)) {
            Closing = true;          // Tell CloseDB of error
            return RC_FX;
            } // endif Vopen

          AddBlock = true;
          } // endif Closing

      } else {
        // Here we must add a new block to the VCT file
        if (Closing)
          // Reset the overwritten columns for last block extra records
          for (; cp; cp = (PVCTCOL)cp->Next)
            memset(NewBlock + Nrec * cp->Deplac + Last * cp->Clen,
                   (cp->Buf_Type == TYPE_STRING) ? ' ' : '\0',
                   (Nrec - Last) * cp->Clen);

        if (BigWrite(g, Hfile, NewBlock, Blksize))
          return RC_FX;

      } // endif AddBlock

      if (!Closing) {
        CurBlk++;
        CurNum = 0;
        } // endif Closing

      } // endif

    } // endif Mode

  return RC_OK;
  } // end of WriteBuffer

/***********************************************************************/
/*  Data Base delete line routine for BGVFAM access method.            */
/***********************************************************************/
int BGVFAM::DeleteRecords(PGLOBAL g, int irc)
  {
  bool eof = false;

  /*********************************************************************/
  /*  There is an alternative here depending on UseTemp:               */
  /*  1 - use a temporary file in which are copied all not deleted     */
  /*      lines, at the end the original file will be deleted and      */
  /*      the temporary file renamed to the original file name.        */
  /*  2 - directly move the not deleted lines inside the original      */
  /*      file, and at the end erase all trailing records.             */
  /*********************************************************************/
  if (trace(1))
    htrc("BGV DeleteDB: irc=%d UseTemp=%d Fpos=%d Tpos=%d Spos=%d\n",
                        irc, UseTemp, Fpos, Tpos, Spos);

  if (irc != RC_OK) {
    /*******************************************************************/
    /*  EOF: position Fpos at the end-of-file position.                */
    /*******************************************************************/
    Fpos = (Block - 1) * Nrec + Last;

    if (trace(1))
      htrc("Fpos placed at file end=%d\n", Fpos);

    eof = UseTemp && !MaxBlk;
  } else     // Fpos is the deleted line position
    Fpos = CurBlk * Nrec + CurNum;

  if (Tpos == Spos) {
    if (UseTemp) {
      /*****************************************************************/
      /*  Open the temporary file, Spos is at the beginning of file.   */
      /*****************************************************************/
      if (OpenTempFile(g))
        return RC_FX;

    } else {
      /*****************************************************************/
      /*  Move of eventual preceding lines is not required here.      */
      /*  Set the target file as being the source file itself.         */
      /*  Set the future Tpos, and give Spos a value to block copying. */
      /*****************************************************************/
      Tfile = Hfile;
      Spos = Tpos = Fpos;
    } // endif UseTemp

    } // endif Tpos == Spos

  /*********************************************************************/
  /*  Move any intermediate lines.                                     */
  /*********************************************************************/
  if (MoveIntermediateLines(g, &eof))
    return RC_FX;

  if (irc == RC_OK) {
#ifdef _DEBUG
    assert(Spos == Fpos);
#endif
    Spos++;          // New start position is on next line

    if (trace(1))
      htrc("after: Tpos=%d Spos=%d\n", Tpos, Spos);

  } else {
    /*******************************************************************/
    /*  Last call after EOF has been reached.                          */
    /*******************************************************************/
    Block = (Tpos > 0) ? (Tpos + Nrec - 1) / Nrec : 0;
    Last = (Tpos + Nrec - 1) % Nrec + 1;

    if (!UseTemp) {    // The UseTemp case is treated in CloseTableFile
      if (!MaxBlk) {
        if (Last < Nrec)            // Clean last block
          if (CleanUnusedSpace(g))
            return RC_FX;

        /***************************************************************/
        /*  Remove extra records.                                      */
        /***************************************************************/
#if defined(_WIN32)
        BIGINT pos = (BIGINT)Block * (BIGINT)Blksize;

        if (BigSeek(g, Hfile, pos))
          return RC_FX;

        if (!SetEndOfFile(Hfile)) {
          DWORD drc = GetLastError();

          sprintf(g->Message, MSG(SETEOF_ERROR), drc);
          return RC_FX;
          } // endif error
#else   // !_WIN32
        if (ftruncate64(Hfile, (BIGINT)(Tpos * Lrecl))) {
          sprintf(g->Message, MSG(TRUNCATE_ERROR), strerror(errno));
          return RC_FX;
          } // endif
#endif  // !_WIN32
      } else // MaxBlk
        // Clean the unused space in the file, this is required when
        // inserting again with a partial column list.
        if (CleanUnusedSpace(g))
          return RC_FX;

      if (ResetTableSize(g, Block, Last))
       return RC_FX;

      } // endif UseTemp

  } // endif irc

  return RC_OK;                                      // All is correct
  } // end of DeleteRecords

/***********************************************************************/
/*  Open a temporary file used while updating or deleting.             */
/***********************************************************************/
bool BGVFAM::OpenTempFile(PGLOBAL g)
  {
  char   *tempname;
  PDBUSER dup = PlgGetUser(g);

  /*********************************************************************/
  /*  Open the temporary file, Spos is at the beginning of file.       */
  /*********************************************************************/
  tempname = (char*)PlugSubAlloc(g, NULL, _MAX_PATH);
  PlugSetPath(tempname, To_File, Tdbp->GetPath());
  strcat(PlugRemoveType(tempname, tempname), ".t");

  if (!MaxBlk)
    remove(tempname);       // Be sure it does not exist yet
  else if (MakeEmptyFile(g, tempname))
    return true;

#if defined(_WIN32)
  DWORD access = (MaxBlk) ? OPEN_EXISTING : CREATE_NEW;

  Tfile = CreateFile(tempname, GENERIC_WRITE, 0, NULL,
                     access, FILE_ATTRIBUTE_NORMAL, NULL);

  if (Tfile == INVALID_HANDLE_VALUE) {
    DWORD rc = GetLastError();
    sprintf(g->Message, MSG(OPEN_ERROR), rc, MODE_DELETE, tempname);
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
              FORMAT_MESSAGE_IGNORE_INSERTS, NULL, rc, 0,
              (LPTSTR)tempname, _MAX_PATH, NULL);
    strcat(g->Message, tempname);
    return true;
    } // endif Tfile
#else    // UNIX
  int oflag = (MaxBlk) ? O_WRONLY : O_WRONLY | O_TRUNC;

  Tfile = open64(tempname, oflag, S_IWRITE);

  if (Tfile == INVALID_HANDLE_VALUE) {
    int rc = errno;
    sprintf(g->Message, MSG(OPEN_ERROR), rc, MODE_INSERT, tempname);
    strcat(g->Message, strerror(errno));
    return true;
    } //endif Tfile
#endif   // UNIX

  To_Fbt = (PFBLOCK)PlugSubAlloc(g, NULL, sizeof(FBLOCK));
  To_Fbt->Fname = tempname;
  To_Fbt->Type = TYPE_FB_HANDLE;
  To_Fbt->Memory = NULL;
  To_Fbt->Length = 0;
  To_Fbt->File = NULL;
  To_Fbt->Next = dup->Openlist;
  To_Fbt->Count = 1;
  To_Fbt->Mode = MODE_INSERT;
  To_Fbt->Handle = Tfile;
  dup->Openlist = To_Fbt;
  return false;
  } // end of OpenTempFile

/***********************************************************************/
/*  Move intermediate deleted or updated lines.                        */
/***********************************************************************/
bool BGVFAM::MoveIntermediateLines(PGLOBAL g, bool *b)
  {
  int    i, n, req, dep;
  bool   eof = (b) ? *b : false;
  BIGINT pos;

  for (n = Fpos - Spos; n > 0 || eof; n -= req) {
    /*******************************************************************/
    /*  Non consecutive line to delete. Move intermediate lines.       */
    /*******************************************************************/
    if (!MaxBlk)
      req = (DWORD)MY_MIN(n, Nrec - MY_MAX(Spos % Nrec, Tpos % Nrec));
    else
      req = (DWORD)MY_MIN(n, Nrec);

    if (req) for (i = 0; i < Ncol; i++) {
      if (!MaxBlk) {
        if (UseTemp)
          To_Buf = NewBlock + Deplac[i] + (Tpos % Nrec) * Clens[i];

        pos = (BIGINT)Deplac[i] + (BIGINT)((Spos % Nrec) * Clens[i])
            + (BIGINT)(Spos / Nrec) * (BIGINT)Blksize;
      } else
        pos = BigDep[i] + (BIGINT)Spos * (BIGINT)Clens[i];

      if (BigSeek(g, Hfile, pos))
        return true;

      if (BigRead(g, Hfile, To_Buf, req * Clens[i]))
        return true;

      if (!UseTemp || MaxBlk) {
        if (!MaxBlk)
          pos = (BIGINT)Deplac[i] + (BIGINT)((Tpos % Nrec) * Clens[i])
              + (BIGINT)(Tpos / Nrec) * (BIGINT)Blksize;
        else
          pos = BigDep[i] + (BIGINT)Tpos * (BIGINT)Clens[i];

        if (BigSeek(g, Tfile, pos))
          return true;

        if (BigWrite(g, Tfile, To_Buf, req * Clens[i]))
          return true;

        } // endif UseTemp

      } // endfor i

    Tpos += (int)req;
    Spos += (int)req;

    if (UseTemp && !MaxBlk && (!(Tpos % Nrec) || (eof && Spos == Fpos))) {
      // Write the full or last block to the temporary file
      if ((dep = Nrec - (Tpos % Nrec)) < Nrec)
        // Clean the last block in case of future insert, must be
        // done here because Tfile was open in write only.
        for (i = 0; i < Ncol; i++) {
          To_Buf = NewBlock + Deplac[i] + (Tpos % Nrec) * Clens[i];
          memset(To_Buf, (Isnum[i]) ? 0 : ' ', dep * Clens[i]);
          } // endfor i

      if (BigWrite(g, Tfile, NewBlock, Blksize))
        return true;

      if (Spos == Fpos)
        eof = false;

      } // endif Usetemp...

    if (trace(1))
      htrc("loop: Tpos=%d Spos=%d\n", Tpos, Spos);

    } // endfor n

  return false;
  } // end of MoveIntermediateLines

/***********************************************************************/
/*  Clean deleted space in a huge VCT or Vec table file.               */
/***********************************************************************/
bool BGVFAM::CleanUnusedSpace(PGLOBAL g)
  {
  int    i;
  int   n;
  BIGINT pos, dep;

  if (!MaxBlk) {
    /*******************************************************************/
    /*  Clean last block of the VCT table file.                        */
    /*******************************************************************/
    assert(!UseTemp); // This case is handled in MoveIntermediateLines

    if (!(n = Nrec - Last))
      return false;

    dep = (BIGINT)((Block - 1) * Blksize);

    for (i = 0; i < Ncol; i++) {
      memset(To_Buf, (Isnum[i]) ? 0 : ' ', n * Clens[i]);
      pos = dep + (BIGINT)(Deplac[i] + Last * Clens[i]);

      if (BigSeek(g, Hfile, pos))
        return true;

      if (BigWrite(g, Hfile, To_Buf, n * Clens[i]))
        return true;

      } // endfor i

  } else {
    int  req;

    memset(To_Buf, 0, Buflen);

    for (n = Fpos - Tpos; n > 0; n -= req) {
      /*****************************************************************/
      /*  Fill VEC file remaining lines with 0's.                      */
      /*  This seems to work even column blocks have been made with    */
      /*  Blanks = true. Perhaps should it be set to false for VEC.    */
      /*****************************************************************/
      req = MY_MIN(n, Nrec);

      for (i = 0; i < Ncol; i++) {
        pos = BigDep[i] + (BIGINT)Tpos * (BIGINT)Clens[i];

        if (BigSeek(g, Tfile, pos))
          return true;

        if (BigWrite(g, Tfile, To_Buf, req * Clens[i]))
          return true;

        } // endfor i

      Tpos += req;
      } // endfor n

  } // endif MaxBlk

  return false;
  } // end of CleanUnusedSpace

/***********************************************************************/
/*  Data Base close routine for huge VEC access method.                */
/***********************************************************************/
void BGVFAM::CloseTableFile(PGLOBAL g, bool abort)
  {
  int  rc = 0, wrc = RC_OK;
  MODE mode = Tdbp->GetMode();

  Abort = abort;

  if (mode == MODE_INSERT) {
    if (Closing)
      wrc = RC_FX;                  // Last write was in error
    else
      if (CurNum) {
        // Some more inserted lines remain to be written
        Last = CurNum;
        Block = CurBlk + 1;
        Closing = true;
        wrc = WriteBuffer(g);
      } else {
        Last = Nrec;
        Block = CurBlk;
        wrc = RC_OK;
      } // endif CurNum

    if (wrc != RC_FX) {
      rc = ResetTableSize(g, Block, Last);
    } else if (AddBlock) {
      // Last block was not written
      rc = ResetTableSize(g, CurBlk, Nrec);
			throw 44;
		} // endif

  } else if (mode == MODE_UPDATE) {
    // Write back to file any pending modifications
    for (PVCTCOL colp = (PVCTCOL)((PTDBVCT)Tdbp)->GetSetCols();
                 colp; colp = (PVCTCOL)colp->Next)
      colp->WriteBlock(g);

    if (UseTemp && Tfile) {
      rc = RenameTempFile(g);
      Hfile = Tfile = INVALID_HANDLE_VALUE;

      if (Header)
        // Header must be set because it was not set in temp file
        rc = SetBlockInfo(g);

      } // endif UseTemp

  } else if (mode == MODE_DELETE && UseTemp && Tfile) {
    if (MaxBlk)
      rc = CleanUnusedSpace(g);

    if ((rc = RenameTempFile(g)) != RC_FX) {
      Hfile = Tfile = INVALID_HANDLE_VALUE;    // For SetBlockInfo
      rc = ResetTableSize(g, Block, Last);
      } // endif rc

  } // endif's mode

  if (Hfile != INVALID_HANDLE_VALUE)
    rc = PlugCloseFile(g, To_Fb);

  if (trace(1))
    htrc("BGV CloseTableFile: closing %s wrc=%d rc=%d\n",
          To_File, wrc, rc);

  Hfile = INVALID_HANDLE_VALUE;
  } // end of CloseDB

/***********************************************************************/
/*  Rewind routine for huge VCT access method.                         */
/***********************************************************************/
void BGVFAM::Rewind(void)
  {
  // In mode update we need to read Set Column blocks
  if (Tdbp->GetMode() == MODE_UPDATE)
    OldBlk = -1;

  // Initialize so block optimization is called for 1st block
  CurBlk = -1;
  CurNum = Nrec - 1;

#if 0 // This is probably unuseful as the file is directly accessed
#if defined(_WIN32)  //OB
  SetFilePointer(Hfile, 0, NULL, FILE_BEGIN);
#else    // UNIX
  lseek64(Hfile, 0, SEEK_SET);
#endif   // UNIX
#endif // 0
  } // end of Rewind

/***********************************************************************/
/*  ReadBlock: Read column values from current block.                  */
/***********************************************************************/
bool BGVFAM::ReadBlock(PGLOBAL g, PVCTCOL colp)
  {
  BIGINT pos;

  /*********************************************************************/
  /*  Calculate the offset and size of the block to read.              */
  /*********************************************************************/
  if (MaxBlk)                                 // File has Vector format
    pos = (BIGINT)Nrec * ((BIGINT)colp->Deplac * (BIGINT)MaxBlk
        + (BIGINT)colp->Clen * (BIGINT)CurBlk) + (BIGINT)Headlen;
  else                                        // Old VCT format
    pos = (BIGINT)Nrec * ((BIGINT)colp->Deplac
        + (BIGINT)Lrecl * (BIGINT)CurBlk);

  if (trace(1))
    htrc("RB: offset=%lld Nrec=%d Deplac=%d Lrecl=%d CurBlk=%d MaxBlk=%d\n",
          pos, Nrec, colp->Deplac, Lrecl, CurBlk, MaxBlk);

  if (BigSeek(g, Hfile, pos))
    return true;

  if (BigRead(g, Hfile, colp->Blk->GetValPointer(), colp->Clen * Nrec))
    return true;

  if (trace(1))
    num_read++;

  return false;
  } // end of ReadBlock

/***********************************************************************/
/*  WriteBlock: Write back current column values for one block.        */
/*  Note: the test of Status is meant to prevent physical writing of   */
/*  the block during the checking loop in mode Update. It is set to    */
/*  BUF_EMPTY when reopening the table between the two loops.          */
/***********************************************************************/
bool BGVFAM::WriteBlock(PGLOBAL g, PVCTCOL colp)
  {
  int    len;
  BIGINT pos;

  /*********************************************************************/
  /*  Calculate the offset and size of the block to write.             */
  /*********************************************************************/
  if (MaxBlk)                               // File has Vector format
    pos = (BIGINT)Nrec * ((BIGINT)colp->Deplac * (BIGINT)MaxBlk
        + (BIGINT)colp->Clen * (BIGINT)colp->ColBlk) + (BIGINT)Headlen;
  else                                      // Old VCT format
    pos = (BIGINT)Nrec * ((BIGINT)colp->Deplac
        + (BIGINT)Lrecl * (BIGINT)colp->ColBlk);

  if (trace(1))
    htrc("WB: offset=%lld Nrec=%d Deplac=%d Lrecl=%d ColBlk=%d\n",
          pos, Nrec, colp->Deplac, Lrecl, colp->ColBlk);

  if (BigSeek(g, Tfile, pos))
    return true;

//len = colp->Clen * Nrec;    see comment in VCTFAM
  len = colp->Clen * ((Tdbp->GetMode() == MODE_INSERT) ? CurNum : Nrec);

  if (BigWrite(g, Tfile, colp->Blk->GetValPointer(), len))
    return true;

  return false;
  } // end of WriteBlock

/* ----------------------- End of FilAMVct --------------------------- */
