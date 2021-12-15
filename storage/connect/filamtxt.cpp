/*********** File AM Txt C++ Program Source Code File (.CPP) ***********/
/* PROGRAM NAME: FILAMTXT                                              */
/* -------------                                                       */
/*  Version 1.8                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2020    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the Text file access method classes.              */
/*                                                                     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the System header files.              */
/***********************************************************************/
#include "my_global.h"
#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif   // __BORLANDC__
//#include <windows.h>
#else   // !_WIN32
#if defined(UNIX) || defined(UNIV_LINUX)
#include <errno.h>
#include <unistd.h>
//#if !defined(sun)                      // Sun has the ftruncate fnc.
//#define USETEMP                        // Force copy mode for DELETE
//#endif   // !sun
#else   // !UNIX
#include <io.h>
#endif  // !UNIX
#include <fcntl.h>
#endif  // !_WIN32

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  filamtxt.h  is header containing the file AM classes declarations. */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "filamtxt.h"
#include "tabdos.h"
#include "tabjson.h"

#if defined(UNIX) || defined(UNIV_LINUX)
#include "osutil.h"
#define _fileno fileno
#define _O_RDONLY O_RDONLY
#endif

extern int num_read, num_there, num_eq[2];               // Statistics

/***********************************************************************/
/*  Routine called externally by TXTFAM SortedRows functions.          */
/***********************************************************************/
PARRAY MakeValueArray(PGLOBAL g, PPARM pp);

/* --------------------------- Class TXTFAM -------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
TXTFAM::TXTFAM(PDOSDEF tdp)
  {
  Tdbp = NULL;
  To_Fb = NULL;

	if (tdp) {
		To_File = tdp->Fn;
		Lrecl = tdp->Lrecl;
		Eof = tdp->Eof;
		Ending = tdp->Ending;
	} else {
		To_File = NULL;
		Lrecl = 0;
		Eof = false;
#if defined(_WIN32)
		Ending = 2;
#else
		Ending = 1;
#endif
	}	// endif tdp

  Placed = false;
  IsRead = true;
  Blocked = false;
  To_Buf = NULL;
  DelBuf = NULL;
  BlkPos = NULL;
  To_Pos = NULL;
  To_Sos = NULL;
  To_Upd = NULL;
  Posar = NULL;
  Sosar = NULL;
  Updar = NULL;
  BlkLen = 0;
  Buflen = 0;
  Dbflen = 0;
  Rows = 0;
  DelRows = 0;
  Headlen = 0;
  Block = 0;
  Last = 0;
  Nrec = 1;
  OldBlk = -1;
  CurBlk = -1;
  ReadBlks = 0;
  CurNum = 0;
  Rbuf = 0;
  Modif = 0;
  Blksize = 0;
  Fpos = Spos = Tpos = 0;
  Padded = false;
  Abort = false;
  CrLf = (char*)(Ending == 1 ? "\n" : "\r\n");
  } // end of TXTFAM standard constructor

TXTFAM::TXTFAM(PTXF txfp)
  {
  Tdbp = txfp->Tdbp;
  To_Fb = txfp->To_Fb;
  To_File = txfp->To_File;
  Lrecl = txfp->Lrecl;
  Placed = txfp->Placed;
  IsRead = txfp->IsRead;
  Blocked = txfp->Blocked;
  To_Buf = txfp->To_Buf;
  DelBuf = txfp->DelBuf;
  BlkPos = txfp->BlkPos;
  To_Pos = txfp->To_Pos;
  To_Sos = txfp->To_Sos;
  To_Upd = txfp->To_Upd;
  Posar = txfp->Posar;
  Sosar = txfp->Sosar;
  Updar = txfp->Updar;
  BlkLen = txfp->BlkLen;
  Buflen = txfp->Buflen;
  Dbflen = txfp->Dbflen;
  Rows = txfp->Rows;
  DelRows = txfp->DelRows;
  Headlen = txfp->Headlen;
  Block = txfp->Block;
  Last = txfp->Last;
  Nrec = txfp->Nrec;
  OldBlk = txfp->OldBlk;
  CurBlk = txfp->CurBlk;
  ReadBlks = txfp->ReadBlks;
  CurNum = txfp->CurNum;
  Rbuf = txfp->Rbuf;
  Modif = txfp->Modif;
  Blksize = txfp->Blksize;
  Fpos = txfp->Fpos;
  Spos = txfp->Spos;
  Tpos = txfp->Tpos;
  Padded = txfp->Padded;
  Eof = txfp->Eof;
  Ending = txfp->Ending;
  Abort = txfp->Abort;
  CrLf = txfp->CrLf;
  } // end of TXTFAM copy constructor

/***********************************************************************/
/*  Reset: reset position values at the beginning of file.             */
/***********************************************************************/
void TXTFAM::Reset(void)
  {
  Rows = 0;
  DelRows = 0;
  OldBlk = -1;
  CurBlk = -1;
  ReadBlks = 0;
  CurNum = 0;
  Rbuf = 0;
  Modif = 0;
  Placed = false;
  } // end of Reset

/***********************************************************************/
/*  TXT GetFileLength: returns file size in number of bytes.           */
/***********************************************************************/
int TXTFAM::GetFileLength(PGLOBAL g)
  {
  char filename[_MAX_PATH];
  int  h;
  int  len;

  PlugSetPath(filename, To_File, Tdbp->GetPath());
  h= global_open(g, MSGID_OPEN_MODE_STRERROR, filename, _O_RDONLY);

  if (trace(1))
    htrc("GetFileLength: fn=%s h=%d\n", filename, h);

  if (h == -1) {
    if (errno != ENOENT) {
      if (trace(1))
        htrc("%s\n", g->Message);

      len = -1;
    } else {
      len = 0;          // File does not exist yet
      g->Message[0]= '\0';
    } // endif errno

  } else {
    if ((len = _filelength(h)) < 0)
      sprintf(g->Message, MSG(FILELEN_ERROR), "_filelength", filename);

    if (Eof && len)
      len--;              // Do not count the EOF character

    close(h);
  } // endif h

  return len;
  } // end of GetFileLength

/***********************************************************************/
/*  Cardinality: returns table cardinality in number of rows.          */
/*  This function can be called with a null argument to test the       */
/*  availability of Cardinality implementation (1 yes, 0 no).          */
/*  Note: This function is meant only for fixed length files but is    */
/*  placed here to be available to FIXFAM and MPXFAM classes.          */
/***********************************************************************/
int TXTFAM::Cardinality(PGLOBAL g)
  {
  if (g) {
    int card = -1;
    int len = GetFileLength(g);

    if (len >= 0) {
      if (Padded && Blksize) {
        if (!(len % Blksize))
          card = (len / Blksize) * Nrec;
        else
          sprintf(g->Message, MSG(NOT_FIXED_LEN), To_File, len, Lrecl);

      } else {
        if (!(len % Lrecl))
          card = len / (int)Lrecl;           // Fixed length file
        else
          sprintf(g->Message, MSG(NOT_FIXED_LEN), To_File, len, Lrecl);

      } // endif Padded

      if (trace(1))
        htrc(" Computed max_K=%d Filen=%d lrecl=%d\n",
              card, len, Lrecl);

    } else
      card = 0;

    // Set number of blocks for later use
    Block = (card > 0) ? (card + Nrec - 1) / Nrec : 0;
    return card;
  } else
    return 1;

  } // end of Cardinality

/***********************************************************************/
/*  Use BlockTest to reduce the table estimated size.                  */
/*  Note: This function is meant only for fixed length files but is    */
/*  placed here to be available to FIXFAM and MPXFAM classes.          */
/***********************************************************************/
int TXTFAM::MaxBlkSize(PGLOBAL g, int s)
  {
  int rc = RC_OK, savcur = CurBlk, blm1 = Block - 1;
  int size, last = s - blm1 * Nrec;

  // Roughly estimate the table size as the sum of blocks
  // that can contain good rows
  for (size = 0, CurBlk = 0; CurBlk < Block; CurBlk++)
    if ((rc = Tdbp->TestBlock(g)) == RC_OK)
      size += (CurBlk == blm1) ? last : Nrec;
    else if (rc == RC_EF)
      break;

  CurBlk = savcur;
  return size;
  } // end of MaxBlkSize

/***********************************************************************/
/*  AddListValue: Used when doing indexed update or delete.            */
/***********************************************************************/
bool TXTFAM::AddListValue(PGLOBAL g, int type, void *val, PPARM *top)
  {
  PPARM pp = (PPARM)PlugSubAlloc(g, NULL, sizeof(PARM));

  switch (type) {
//  case TYPE_INT:
//    pp->Value = PlugSubAlloc(g, NULL, sizeof(int));
//    *((int*)pp->Value) = *((int*)val);
//    break;
    case TYPE_VOID:
      pp->Intval = *(int*)val;
      break;
//  case TYPE_STRING:
//    pp->Value = PlugDup(g, (char*)val);
//    break;
    case TYPE_PCHAR:
      pp->Value = val;
      break;
    default:
      return true;
  } // endswitch type

  pp->Type = type;
  pp->Domain = 0;
  pp->Next = *top;
  *top = pp;
  return false;
  } // end of AddListValue

/***********************************************************************/
/*  Store needed values for indexed UPDATE or DELETE.                  */
/***********************************************************************/
int TXTFAM::StoreValues(PGLOBAL g, bool upd)
{
  int  pos = GetPos();
  bool rc = AddListValue(g, TYPE_VOID, &pos, &To_Pos);

  if (!rc) {
    pos = GetNextPos();
    rc = AddListValue(g, TYPE_VOID, &pos, &To_Sos);
    } // endif rc

  if (upd && !rc) {
    char *buf;

    if (Tdbp->PrepareWriting(g))
      return RC_FX;

    buf = PlugDup(g, Tdbp->GetLine());
    rc = AddListValue(g, TYPE_PCHAR, buf, &To_Upd);
    } // endif upd

  return rc ? RC_FX : RC_OK;
} // end of StoreValues

/***********************************************************************/
/*  UpdateSortedRows. When updating using indexing, the issue is that  */
/*  record are not necessarily updated in sequential order.            */
/*  Moving intermediate lines cannot be done while making them because */
/*  this can cause extra wrong records to be included in the new file. */
/*  What we do here is to reorder the updated records and do all the   */
/*  updates ordered by record position.                                */
/***********************************************************************/
int TXTFAM::UpdateSortedRows(PGLOBAL g)
  {
  int  *ix, i;

  /*********************************************************************/
  /*  Get the stored update values and sort them.                      */
  /*********************************************************************/
  if (!(Posar = MakeValueArray(g, To_Pos))) {
//  strcpy(g->Message, "Position array is null");
//  return RC_INFO;
    return RC_OK;         // Nothing to do
  } else if (!(Sosar = MakeValueArray(g, To_Sos))) {
    strcpy(g->Message, "Start position array is null");
    goto err;
  } else if (!(Updar = MakeValueArray(g, To_Upd))) {
    strcpy(g->Message, "Updated line array is null");
    goto err;
  } else if (!(ix = (int*)Posar->GetSortIndex(g))) { 
    strcpy(g->Message, "Error getting array sort index");
    goto err;
  } // endif's

  Rewind();

  for (i = 0; i < Posar->GetNval(); i++) {
    SetPos(g, Sosar->GetIntValue(ix[i]));
    Fpos = Posar->GetIntValue(ix[i]);
    strcpy(Tdbp->To_Line, Updar->GetStringValue(ix[i]));

    // Now write the updated line.
    if (WriteBuffer(g))
      goto err;

    } // endfor i

  return RC_OK;

err:
  if (trace(1))
    htrc("%s\n", g->Message);

  return RC_FX;
  } // end of UpdateSortedRows

/***********************************************************************/
/*  DeleteSortedRows. When deleting using indexing, the issue is that  */
/*  record are not necessarily deleted in sequential order. Moving     */
/*  intermediate lines cannot be done while deleing them because       */
/*  this can cause extra wrong records to be included in the new file. */
/*  What we do here is to reorder the deleted record and delete from   */
/*  the file from the ordered deleted records.                         */
/***********************************************************************/
int TXTFAM::DeleteSortedRows(PGLOBAL g)
  {
  int  *ix, i, irc;

  /*********************************************************************/
  /*  Get the stored delete values and sort them.                      */
  /*********************************************************************/
  if (!(Posar = MakeValueArray(g, To_Pos))) {
//  strcpy(g->Message, "Position array is null");
//  return RC_INFO;
    return RC_OK;             // Nothing to do
  } else if (!(Sosar = MakeValueArray(g, To_Sos))) {
    strcpy(g->Message, "Start position array is null");
    goto err;
  } else if (!(ix = (int*)Posar->GetSortIndex(g))) { 
    strcpy(g->Message, "Error getting array sort index");
    goto err;
  } // endif's

  Tpos = Spos = 0;

  for (i = 0; i < Posar->GetNval(); i++) {
    if ((irc = InitDelete(g, Posar->GetIntValue(ix[i]), 
                             Sosar->GetIntValue(ix[i]))) == RC_FX)
      goto err;

    // Now delete the sorted rows
    if (DeleteRecords(g, irc))
      goto err;

    } // endfor i

  return RC_OK;

err:
  if (trace(1))
    htrc("%s\n", g->Message);

  return RC_FX;
  } // end of DeleteSortedRows

/***********************************************************************/
/*  The purpose of this function is to deal with access methods that   */
/*  are not coherent regarding the use of SetPos and GetPos.           */
/***********************************************************************/
int TXTFAM::InitDelete(PGLOBAL g, int, int)
  {
  strcpy(g->Message, "InitDelete should not be used by this table type");
  return RC_FX;
  } // end of InitDelete

/* --------------------------- Class DOSFAM -------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
DOSFAM::DOSFAM(PDOSDEF tdp) : TXTFAM(tdp)
  {
  To_Fbt = NULL;
  Stream = NULL;
  T_Stream = NULL;
  UseTemp = false;
  Bin = false;
  } // end of DOSFAM standard constructor

DOSFAM::DOSFAM(PDOSFAM tdfp) : TXTFAM(tdfp)
  {
  To_Fbt = tdfp->To_Fbt;
  Stream = tdfp->Stream;
  T_Stream = tdfp->T_Stream;
  UseTemp = tdfp->UseTemp;
  Bin = tdfp->Bin;
  } // end of DOSFAM copy constructor

DOSFAM::DOSFAM(PBLKFAM tdfp, PDOSDEF tdp) : TXTFAM(tdp)
  {
  Tdbp = tdfp->Tdbp;
  To_Fb = tdfp->To_Fb;
  To_Fbt = tdfp->To_Fbt;
  Stream = tdfp->Stream;
  T_Stream = tdfp->T_Stream;
  UseTemp = tdfp->UseTemp;
  Bin = tdfp->Bin;
  } // end of DOSFAM constructor from BLKFAM

/***********************************************************************/
/*  Reset: reset position values at the beginning of file.             */
/***********************************************************************/
void DOSFAM::Reset(void)
  {
  TXTFAM::Reset();
  Bin = false;
  Fpos = Tpos = Spos = 0;
  } // end of Reset

/***********************************************************************/
/*  DOS GetFileLength: returns file size in number of bytes.           */
/***********************************************************************/
int DOSFAM::GetFileLength(PGLOBAL g)
  {
  int len;

  if (!Stream)
    len = TXTFAM::GetFileLength(g);
  else
    if ((len = _filelength(_fileno(Stream))) < 0)
      sprintf(g->Message, MSG(FILELEN_ERROR), "_filelength", To_File);

  if (trace(1))
    htrc("File length=%d\n", len);

  return len;
  } // end of GetFileLength

/***********************************************************************/
/*  Cardinality: returns table cardinality in number of rows.          */
/*  This function can be called with a null argument to test the       */
/*  availability of Cardinality implementation (1 yes, 0 no).          */
/***********************************************************************/
int DOSFAM::Cardinality(PGLOBAL g)
  {
  return (g) ? -1 : 0;
  } // end of Cardinality

/***********************************************************************/
/*  Use BlockTest to reduce the table estimated size.                  */
/*  Note: This function is not really implemented yet.                 */
/***********************************************************************/
int DOSFAM::MaxBlkSize(PGLOBAL, int s)
  {
  return s;
  } // end of MaxBlkSize

/***********************************************************************/
/*  OpenTableFile: Open a DOS/UNIX table file using C standard I/Os.   */
/***********************************************************************/
bool DOSFAM::OpenTableFile(PGLOBAL g)
  {
  char    opmode[4], filename[_MAX_PATH];
//int     ftype = Tdbp->GetFtype();
  MODE    mode = Tdbp->Mode;
  PDBUSER dbuserp = PlgGetUser(g);

  // This is required when using Unix files under Windows and vice versa
//Bin = (Blocked || Ending != CRLF);
  Bin = true;             // To avoid ftell problems

  switch (mode) {
    case MODE_READ:
      strcpy(opmode, "r");
      break;
    case MODE_DELETE:
      if (!Tdbp->Next) {
        // Store the number of deleted lines
        DelRows = Cardinality(g);

        if (Blocked) {
          // Cardinality must return 0
          Block = 0;
          Last = Nrec;
          } // endif blocked

        // This will erase the entire file
        strcpy(opmode, "w");
        Tdbp->ResetSize();
        break;
        } // endif

      // Selective delete, pass thru
      Bin = true;
      /* fall through */
    case MODE_UPDATE:
      if ((UseTemp = Tdbp->IsUsingTemp(g))) {
        strcpy(opmode, "r");
        Bin = true;
      } else
        strcpy(opmode, "r+");

      break;
    case MODE_INSERT:
      strcpy(opmode, "a+");
      break;
    default:
      sprintf(g->Message, MSG(BAD_OPEN_MODE), mode);
      return true;
    } // endswitch Mode

  // For blocked I/O or for moving lines, open the table in binary
  strcat(opmode, (Bin) ? "b" : "t");

  // Now open the file stream
  PlugSetPath(filename, To_File, Tdbp->GetPath());

  if (!(Stream = PlugOpenFile(g, filename, opmode))) {
    if (trace(1))
      htrc("%s\n", g->Message);

    return (mode == MODE_READ && errno == ENOENT)
            ? PushWarning(g, Tdbp) : true;
    } // endif Stream

  if (trace(1))
    htrc("File %s open Stream=%p mode=%s\n", filename, Stream, opmode);

  To_Fb = dbuserp->Openlist;     // Keep track of File block

  /*********************************************************************/
  /*  Allocate the line buffer. For mode Delete a bigger buffer has to */
  /*  be allocated because is it also used to move lines into the file.*/
  /*********************************************************************/
  return AllocateBuffer(g);
  } // end of OpenTableFile

/***********************************************************************/
/*  Allocate the line buffer. For mode Delete a bigger buffer has to   */
/*  be allocated because is it also used to move lines into the file.  */
/***********************************************************************/
bool DOSFAM::AllocateBuffer(PGLOBAL g)
  {
  MODE mode = Tdbp->Mode;

  // Lrecl does not include line ending
  Buflen = Lrecl + Ending + ((Bin) ? 1 : 0) + 1;     // Sergei

  if (trace(1))
    htrc("SubAllocating a buffer of %d bytes\n", Buflen);

  To_Buf = (char*)PlugSubAlloc(g, NULL, Buflen);

  if (UseTemp || mode == MODE_DELETE) {
    // Have a big buffer to move lines
    Dbflen = Buflen * DOS_BUFF_LEN;
    DelBuf = PlugSubAlloc(g, NULL, Dbflen);
  } else if (mode == MODE_INSERT) {
    /*******************************************************************/
    /*  Prepare the buffer so eventual gaps are filled with blanks.    */
    /*******************************************************************/
    memset(To_Buf, ' ', Buflen);
    To_Buf[Buflen - 2] = '\n';
    To_Buf[Buflen - 1] = '\0';
  } // endif's mode

  return false;
  } // end of AllocateBuffer

/***********************************************************************/
/*  GetRowID: return the RowID of last read record.                    */
/***********************************************************************/
int DOSFAM::GetRowID(void)
  {
  return Rows;
  } // end of GetRowID

/***********************************************************************/
/*  GetPos: return the position of last read record.                   */
/***********************************************************************/
int DOSFAM::GetPos(void)
  {
  return Fpos;
  } // end of GetPos

/***********************************************************************/
/*  GetNextPos: return the position of next record.                    */
/***********************************************************************/
int DOSFAM::GetNextPos(void)
  {
  return ftell(Stream);
  } // end of GetNextPos

/***********************************************************************/
/*  SetPos: Replace the table at the specified position.               */
/***********************************************************************/
bool DOSFAM::SetPos(PGLOBAL g, int pos)
  {
  Fpos = pos;

  if (fseek(Stream, Fpos, SEEK_SET)) {
    sprintf(g->Message, MSG(FSETPOS_ERROR), Fpos);
    return true;
    } // endif

  Placed = true;
  return false;
  } // end of SetPos

/***********************************************************************/
/*  Record file position in case of UPDATE or DELETE.                  */
/***********************************************************************/
bool DOSFAM::RecordPos(PGLOBAL g)
  {
  if ((Fpos = ftell(Stream)) < 0) {
    sprintf(g->Message, MSG(FTELL_ERROR), 0, strerror(errno));
//  strcat(g->Message, " (possible wrong ENDING option value)");
    return true;
    } // endif Fpos

  return false;
  } // end of RecordPos

/***********************************************************************/
/*  Initialize Fpos and the current position for indexed DELETE.       */
/***********************************************************************/
int DOSFAM::InitDelete(PGLOBAL g, int fpos, int spos)
  {
  Fpos = fpos;

  if (fseek(Stream, spos, SEEK_SET)) {
    sprintf(g->Message, MSG(FSETPOS_ERROR), Fpos);
    return RC_FX;
    } // endif

  return RC_OK;
  } // end of InitDelete

/***********************************************************************/
/*  Skip one record in file.                                           */
/***********************************************************************/
int DOSFAM::SkipRecord(PGLOBAL g, bool header)
  {
  PDBUSER dup = (PDBUSER)g->Activityp->Aptr;

  // Skip this record
  if (!fgets(To_Buf, Buflen, Stream)) {
    if (feof(Stream))
      return RC_EF;

#if defined(_WIN32)
    sprintf(g->Message, MSG(READ_ERROR), To_File, _strerror(NULL));
#else
    sprintf(g->Message, MSG(READ_ERROR), To_File, strerror(0));
#endif
    return RC_FX;
    } // endif fgets

  // Update progress information
  dup->ProgCur = GetPos();

  if (header) {
    // For Delete
    Fpos = ftell(Stream);

    if (!UseTemp)
      Tpos = Spos = Fpos;     // No need to move header

    } // endif header

#if defined(THREAD)
  return RC_NF;                  // To have progress info
#else
  return RC_OK;                  // To loop locally
#endif
  } // end of SkipRecord

/***********************************************************************/
/*  ReadBuffer: Read one line for a text file.                         */
/***********************************************************************/
int DOSFAM::ReadBuffer(PGLOBAL g)
  {
  char *p;
  int   rc;

  if (!Stream)
    return RC_EF;

  if (trace(2))
    htrc("ReadBuffer: Tdbp=%p To_Line=%p Placed=%d\n",
                      Tdbp, Tdbp->To_Line, Placed);

  if (!Placed) {
    /*******************************************************************/
    /*  Record file position in case of UPDATE or DELETE.              */
    /*******************************************************************/
   next:
    if (RecordPos(g))
      return RC_FX;

    CurBlk = (int)Rows++;

    if (trace(2))
      htrc("ReadBuffer: CurBlk=%d\n", CurBlk);

   /********************************************************************/
    /*  Check whether optimization on ROWID                            */
    /*  can be done, as well as for join as for local filtering.       */
    /*******************************************************************/
    switch (Tdbp->TestBlock(g)) {
      case RC_EF:
        return RC_EF;
      case RC_NF:
        // Skip this record
        if ((rc = SkipRecord(g, FALSE)) != RC_OK)
          return rc;

        goto next;
      } // endswitch rc

  } else
    Placed = false;

  if (trace(2))
    htrc(" About to read: stream=%p To_Buf=%p Buflen=%d Fpos=%d\n",
                          Stream, To_Buf, Buflen, Fpos);

  if (fgets(To_Buf, Buflen, Stream)) {
    p = To_Buf + strlen(To_Buf) - 1;

    if (trace(2))
      htrc(" Read: To_Buf=%p p=%c\n", To_Buf, p);

#if defined(_WIN32)
    if (Bin) {
      // Data file is read in binary so CRLF remains
#else
    if (true) {
      // Data files can be imported from Windows (having CRLF)
#endif
      if (*p == '\n' || *p == '\r') {
        // is this enough for Unix ???
        *p = '\0';          // Eliminate ending CR or LF character

        if (p > To_Buf) {
          // is this enough for Unix ???
          p--;

          if (*p == '\n' || *p == '\r')
            *p = '\0';      // Eliminate ending CR or LF character

          } // endif To_Buf

        } // endif p

    } else if (*p == '\n')
      *p = '\0';          // Eliminate ending new-line character

    if (trace(2))
      htrc(" To_Buf='%s'\n", To_Buf);

    strcpy(Tdbp->To_Line, To_Buf);
    num_read++;
    rc = RC_OK;
  } else if (feof(Stream)) {
    rc = RC_EF;
  } else {
#if defined(_WIN32)
    sprintf(g->Message, MSG(READ_ERROR), To_File, _strerror(NULL));
#else
    sprintf(g->Message, MSG(READ_ERROR), To_File, strerror(0));
#endif

    if (trace(1))
      htrc("%s\n", g->Message);

    rc = RC_FX;
  } // endif's fgets

  if (trace(2))
    htrc("ReadBuffer: rc=%d\n", rc);

  IsRead = true;
  return rc;
  } // end of ReadBuffer

/***********************************************************************/
/*  WriteBuffer: File write routine for DOS access method.             */
/***********************************************************************/
int DOSFAM::WriteBuffer(PGLOBAL g)
  {
  int   curpos = 0;
  bool  moved = true;

  // T_Stream is the temporary stream or the table file stream itself
  if (!T_Stream) {
    if (UseTemp && Tdbp->Mode == MODE_UPDATE) {
      if (OpenTempFile(g))
        return RC_FX;

    } else
      T_Stream = Stream;

    } // endif T_Stream

  if (Tdbp->Mode == MODE_UPDATE) {
    /*******************************************************************/
    /*  Here we simply rewrite a record on itself. There are two cases */
    /*  were another method should be used, a/ when Update apply to    */
    /*  the whole file, b/ when updating the last field of a variable  */
    /*  length file. The method could be to rewrite a new file, then   */
    /*  to erase the old one and rename the new updated file.          */
    /*******************************************************************/
    curpos = ftell(Stream);

    if (trace(1))
      htrc("Last : %d cur: %d\n", Fpos, curpos);

    if (UseTemp) {
      /*****************************************************************/
      /*  We are using a temporary file.                               */
      /*  Before writing the updated record, we must eventually copy   */
      /*  all the intermediate records that have not been updated.     */
      /*****************************************************************/
      if (MoveIntermediateLines(g, &moved))
        return RC_FX;

      Spos = curpos;                            // New start position
    } else
      // Update is directly written back into the file,
      //   with this (fast) method, record size cannot change.
      if (fseek(Stream, Fpos, SEEK_SET)) {
        sprintf(g->Message, MSG(FSETPOS_ERROR), 0);
        return RC_FX;
        } // endif

    } // endif mode

  /*********************************************************************/
  /*  Prepare the write the updated line.                              */
  /*********************************************************************/
  strcat(strcpy(To_Buf, Tdbp->To_Line), (Bin) ? CrLf : "\n");

  /*********************************************************************/
  /*  Now start the writing process.                                   */
  /*********************************************************************/
  if ((fputs(To_Buf, T_Stream)) == EOF) {
    sprintf(g->Message, MSG(FPUTS_ERROR), strerror(errno));
    return RC_FX;
    } // endif EOF

  if (Tdbp->Mode == MODE_UPDATE && moved)
    if (fseek(Stream, curpos, SEEK_SET)) {
      sprintf(g->Message, MSG(FSEEK_ERROR), strerror(errno));
      return RC_FX;
      } // endif

  if (trace(1))
    htrc("write done\n");

  return RC_OK;
  } // end of WriteBuffer

/***********************************************************************/
/*  Data Base delete line routine for DOS and BLK access methods.      */
/***********************************************************************/
int DOSFAM::DeleteRecords(PGLOBAL g, int irc)
  {
  bool moved;
  int  curpos = ftell(Stream);

  /*********************************************************************/
  /*  There is an alternative here:                                    */
  /*  1 - use a temporary file in which are copied all not deleted     */
  /*      lines, at the end the original file will be deleted and      */
  /*      the temporary file renamed to the original file name.        */
  /*  2 - directly move the not deleted lines inside the original      */
  /*      file, and at the end erase all trailing records.             */
  /*  This will be experimented.                                       */
  /*********************************************************************/
  if (trace(1))
    htrc(
  "DOS DeleteDB: rc=%d UseTemp=%d curpos=%d Fpos=%d Tpos=%d Spos=%d\n",
                irc, UseTemp, curpos, Fpos, Tpos, Spos);

  if (irc != RC_OK) {
    /*******************************************************************/
    /*  EOF: position Fpos at the end-of-file position.                */
    /*******************************************************************/
    fseek(Stream, 0, SEEK_END);
    Fpos = ftell(Stream);

    if (trace(1))
      htrc("Fpos placed at file end=%d\n", Fpos);

    } // endif irc

  if (Tpos == Spos) {
    /*******************************************************************/
    /*  First line to delete, Open temporary file.                     */
    /*******************************************************************/
    if (UseTemp) {
      if (OpenTempFile(g))
        return RC_FX;

    } else {
      /*****************************************************************/
      /*  Move of eventual preceding lines is not required here.       */
      /*  Set the target file as being the source file itself.         */
      /*  Set the future Tpos, and give Spos a value to block copying. */
      /*****************************************************************/
      T_Stream = Stream;
      Spos = Tpos = Fpos;
    } // endif UseTemp

    } // endif Tpos == Spos

  /*********************************************************************/
  /*  Move any intermediate lines.                                     */
  /*********************************************************************/
  if (MoveIntermediateLines(g, &moved))
    return RC_FX;

  if (irc == RC_OK) {
    /*******************************************************************/
    /*  Reposition the file pointer and set Spos.                      */
    /*******************************************************************/
    if (!UseTemp || moved)
      if (fseek(Stream, curpos, SEEK_SET)) {
        sprintf(g->Message, MSG(FSETPOS_ERROR), 0);
        return RC_FX;
        } // endif

    Spos = GetNextPos();                     // New start position

    if (trace(1))
      htrc("after: Tpos=%d Spos=%d\n", Tpos, Spos);

  } else {
    /*******************************************************************/
    /*  Last call after EOF has been reached.                          */
    /*  The UseTemp case is treated in CloseTableFile.                 */
    /*******************************************************************/
    if (!UseTemp & !Abort) {
      /*****************************************************************/
      /*  Because the chsize functionality is only accessible with a   */
      /*  system call we must close the file and reopen it with the    */
      /*  open function (_fopen for MS ??) this is still to be checked */
      /*  for compatibility with Text files and other OS's.            */
      /*****************************************************************/
      char filename[_MAX_PATH];
      int  h;                           // File handle, return code

      PlugSetPath(filename, To_File, Tdbp->GetPath());
      /*rc=*/ PlugCloseFile(g, To_Fb);

      if ((h= global_open(g, MSGID_OPEN_STRERROR, filename, O_WRONLY)) <= 0)
        return RC_FX;

      /*****************************************************************/
      /*  Remove extra records.                                        */
      /*****************************************************************/
#if defined(_WIN32)
      if (chsize(h, Tpos)) {
        sprintf(g->Message, MSG(CHSIZE_ERROR), strerror(errno));
        close(h);
        return RC_FX;
        } // endif
#else
      if (ftruncate(h, (off_t)Tpos)) {
        sprintf(g->Message, MSG(TRUNCATE_ERROR), strerror(errno));
        close(h);
        return RC_FX;
        } // endif
#endif

      close(h);

      if (trace(1))
        htrc("done, h=%d irc=%d\n", h, irc);

      } // endif !UseTemp

  } // endif irc

  return RC_OK;                                      // All is correct
  } // end of DeleteRecords

/***********************************************************************/
/*  Open a temporary file used while updating or deleting.             */
/***********************************************************************/
bool DOSFAM::OpenTempFile(PGLOBAL g)
  {
  char tempname[_MAX_PATH];
  bool rc = false;

  /*********************************************************************/
  /*  Open the temporary file, Spos is at the beginning of file.       */
  /*********************************************************************/
  PlugSetPath(tempname, To_File, Tdbp->GetPath());
  strcat(PlugRemoveType(tempname, tempname), ".t");

  if (!(T_Stream = PlugOpenFile(g, tempname, "wb"))) {
    if (trace(1))
      htrc("%s\n", g->Message);

    rc = true;
  } else
    To_Fbt = PlgGetUser(g)->Openlist;

  return rc;
  } // end of OpenTempFile

/***********************************************************************/
/*  Move intermediate deleted or updated lines.                        */
/*  This works only for file open in binary mode.                      */
/***********************************************************************/
bool DOSFAM::MoveIntermediateLines(PGLOBAL g, bool *b)
  {
  int   n;
  size_t req, len;

  for (*b = false, n = Fpos - Spos; n > 0; n -= req) {
    if (!UseTemp || !*b)
      if (fseek(Stream, Spos, SEEK_SET)) {
        sprintf(g->Message, MSG(READ_SEEK_ERROR), strerror(errno));
        return true;
        } // endif

    req = (size_t)MY_MIN(n, Dbflen);
    len = fread(DelBuf, 1, req, Stream);

    if (trace(1))
      htrc("after read req=%d len=%d\n", req, len);

    if (len != req) {
      sprintf(g->Message, MSG(DEL_READ_ERROR), (int) req, (int) len);
      return true;
      } // endif len

    if (!UseTemp)
      if (fseek(T_Stream, Tpos, SEEK_SET)) {
        sprintf(g->Message, MSG(WRITE_SEEK_ERR), strerror(errno));
        return true;
        } // endif

    if ((len = fwrite(DelBuf, 1, req, T_Stream)) != req) {
      sprintf(g->Message, MSG(DEL_WRITE_ERROR), strerror(errno));
      return true;
      } // endif

    if (trace(1))
      htrc("after write pos=%d\n", ftell(Stream));

    Tpos += (int)req;
    Spos += (int)req;

    if (trace(1))
      htrc("loop: Tpos=%d Spos=%d\n", Tpos, Spos);

    *b = true;
    } // endfor n

  return false;
  } // end of MoveIntermediate Lines

/***********************************************************************/
/*  Delete the old file and rename the new temp file.                  */
/*  If aborting just delete the new temp file.                         */
/*  If indexed, make the temp file from the arrays.                    */
/***********************************************************************/
int DOSFAM::RenameTempFile(PGLOBAL g)
  {
  char *tempname, filetemp[_MAX_PATH], filename[_MAX_PATH];
  int   rc = RC_OK;

  if (To_Fbt)
    tempname = (char*)To_Fbt->Fname;
  else
    return RC_INFO;               // Nothing to do ???

  // This loop is necessary because, in case of join,
  // To_File can have been open several times.
  for (PFBLOCK fb = PlgGetUser(g)->Openlist; fb; fb = fb->Next)
    if (fb == To_Fb || (fb == To_Fbt))
      rc = PlugCloseFile(g, fb);
  
  if (!Abort) {
    PlugSetPath(filename, To_File, Tdbp->GetPath());
    strcat(PlugRemoveType(filetemp, filename), ".ttt");
    remove(filetemp);   // May still be there from previous error

    if (rename(filename, filetemp)) {    // Save file for security
      snprintf(g->Message, MAX_STR, MSG(RENAME_ERROR),
              filename, filetemp, strerror(errno));
			throw 51;
		} else if (rename(tempname, filename)) {
      snprintf(g->Message, MAX_STR, MSG(RENAME_ERROR),
              tempname, filename, strerror(errno));
      rc = rename(filetemp, filename);   // Restore saved file
			throw 52;
		} else if (remove(filetemp)) {
      sprintf(g->Message, MSG(REMOVE_ERROR),
              filetemp, strerror(errno));
      rc = RC_INFO;                      // Acceptable
    } // endif's

  } else
    remove(tempname);

  return rc;
  } // end of RenameTempFile

/***********************************************************************/
/*  Table file close routine for DOS access method.                    */
/***********************************************************************/
void DOSFAM::CloseTableFile(PGLOBAL g, bool abort)
  {
  int rc;

  Abort = abort;

  if (UseTemp && T_Stream) {
    if (Tdbp->Mode == MODE_UPDATE && !Abort) {
      // Copy eventually remaining lines
      bool b;

      fseek(Stream, 0, SEEK_END);
      Fpos = ftell(Stream);
      Abort = MoveIntermediateLines(g, &b) != RC_OK;
      } // endif Abort

    // Delete the old file and rename the new temp file.
    rc = RenameTempFile(g);     // Also close all files
  } else {
    rc = PlugCloseFile(g, To_Fb);

    if (trace(1))
      htrc("DOS Close: closing %s rc=%d\n", To_File, rc);

  } // endif UseTemp

  Stream = NULL;           // So we can know whether table is open
  T_Stream = NULL;
  } // end of CloseTableFile

/***********************************************************************/
/*  Rewind routine for DOS access method.                              */
/***********************************************************************/
void DOSFAM::Rewind(void)
  {
  if (Stream)  // Can be NULL when making index on void table
    rewind(Stream);

  Rows = 0;
  OldBlk = CurBlk = -1;
  } // end of Rewind

/* --------------------------- Class BLKFAM -------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
BLKFAM::BLKFAM(PDOSDEF tdp) : DOSFAM(tdp)
  {
  Blocked = true;
  Block = tdp->GetBlock();
  Last = tdp->GetLast();
  Nrec = tdp->GetElemt();
  Closing = false;
  BlkPos = tdp->GetTo_Pos();
  CurLine = NULL;
  NxtLine = NULL;
  OutBuf = NULL;
  } // end of BLKFAM standard constructor

BLKFAM::BLKFAM(PBLKFAM txfp) : DOSFAM(txfp)
  {
  Closing = txfp->Closing;
  CurLine = txfp->CurLine;
  NxtLine = txfp->NxtLine;
  OutBuf = txfp->OutBuf;
  } // end of BLKFAM copy constructor

/***********************************************************************/
/*  Reset: reset position values at the beginning of file.             */
/***********************************************************************/
void BLKFAM::Reset(void)
  {
  DOSFAM::Reset();
  Closing = false;
  } // end of Reset

/***********************************************************************/
/*  Cardinality: returns table cardinality in number of rows.          */
/*  This function can be called with a null argument to test the       */
/*  availability of Cardinality implementation (1 yes, 0 no).          */
/***********************************************************************/
int BLKFAM::Cardinality(PGLOBAL g)
  {
  return (g) ? ((Block > 0) ? (int)((Block - 1) * Nrec + Last) : 0) : 1;
  } // end of Cardinality

/***********************************************************************/
/*  Use BlockTest to reduce the table estimated size.                  */
/***********************************************************************/
int BLKFAM::MaxBlkSize(PGLOBAL g, int)
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
/*  Allocate the line buffer. For mode Delete or when a temp file is   */
/*  used another big buffer has to be allocated because is it used     */
/*  to move or update the lines into the (temp) file.                  */
/***********************************************************************/
bool BLKFAM::AllocateBuffer(PGLOBAL g)
  {
  int  len;
  MODE mode = Tdbp->GetMode();

  // For variable length files, Lrecl does not include CRLF
  len = Lrecl + ((Tdbp->GetFtype()) ? 0 : Ending);
  Buflen = len * Nrec;
  CurLine = To_Buf = (char*)PlugSubAlloc(g, NULL, Buflen);

  if (UseTemp || mode == MODE_DELETE) {
    if (mode == MODE_UPDATE)
      OutBuf = (char*)PlugSubAlloc(g, NULL, len + 1);

    Dbflen = Buflen;
    DelBuf = PlugSubAlloc(g, NULL, Dbflen);
  } else if (mode == MODE_INSERT)
    Rbuf = Nrec;                     // To be used by WriteDB

  return false;
  } // end of AllocateBuffer

/***********************************************************************/
/*  GetRowID: return the RowID of last read record.                    */
/***********************************************************************/
int BLKFAM::GetRowID(void)
  {
  return CurNum + Nrec * CurBlk + 1;
  } // end of GetRowID

/***********************************************************************/
/*  GetPos: return the position of last read record.                   */
/***********************************************************************/
int BLKFAM::GetPos(void)
  {
  return (CurNum + Nrec * CurBlk);          // Computed file index
  } // end of GetPos

/***********************************************************************/
/*  GetNextPos: called by DeleteRecords.                               */
/***********************************************************************/
int BLKFAM::GetNextPos(void)
  {
  return (int)(Fpos + NxtLine - CurLine);
  } // end of GetNextPos

/***********************************************************************/
/*  SetPos: Replace the table at the specified position.               */
/***********************************************************************/
bool BLKFAM::SetPos(PGLOBAL g, int)
  {
  strcpy(g->Message, "Blocked variable tables cannot be used indexed");
  return true;
  } // end of SetPos

/***********************************************************************/
/*  Record file position in case of UPDATE or DELETE.                  */
/*  Not used yet for blocked tables.                                   */
/***********************************************************************/
bool BLKFAM::RecordPos(PGLOBAL)
  {
  Fpos = (CurNum + Nrec * CurBlk);          // Computed file index
  return false;
  } // end of RecordPos

/***********************************************************************/
/*  Skip one record in file.                                           */
/***********************************************************************/
int BLKFAM::SkipRecord(PGLOBAL, bool header)
  {
  if (header) {
    // For Delete
    Fpos = BlkPos[0];         // First block starts after the header

    if (!UseTemp)
      Tpos = Spos = Fpos;     // No need to move header

    } // endif header

  OldBlk = -2;        // To force fseek on first block
  return RC_OK;
  } // end of SkipRecord

/***********************************************************************/
/*  ReadBuffer: Read one line for a text file.                         */
/***********************************************************************/
int BLKFAM::ReadBuffer(PGLOBAL g)
  {
  int i, rc = RC_OK;
  size_t n;

  /*********************************************************************/
  /*  Sequential reading when Placed is not true.                      */
  /*********************************************************************/
  if (Placed) {
    Placed = false;
  } else if (++CurNum < Rbuf) {
    CurLine = NxtLine;

    // Get the position of the next line in the buffer
    while (*NxtLine++ != '\n') ;

    // Set caller line buffer
    n = NxtLine - CurLine - Ending;
    memcpy(Tdbp->GetLine(), CurLine, n);
    Tdbp->GetLine()[n] = '\0';
    goto fin;
  } else if (Rbuf < Nrec && CurBlk != -1) {
    return RC_EF;
  } else {
    /*******************************************************************/
    /*  New block.                                                     */
    /*******************************************************************/
    CurNum = 0;

   next:
    if (++CurBlk >= Block)
      return RC_EF;

    /*******************************************************************/
    /*  Before reading a new block, check whether block optimization   */
    /*  can be done, as well as for join as for local filtering.       */
    /*******************************************************************/
    switch (Tdbp->TestBlock(g)) {
      case RC_EF:
        return RC_EF;
      case RC_NF:
        goto next;
      } // endswitch rc

  } // endif's

  if (OldBlk == CurBlk)
    goto ok;         // Block is already there

  // fseek is required only in non sequential reading
  if (CurBlk != OldBlk + 1)
    if (fseek(Stream, BlkPos[CurBlk], SEEK_SET)) {
      sprintf(g->Message, MSG(FSETPOS_ERROR), BlkPos[CurBlk]);
      return RC_FX;
      } // endif fseek

  // Calculate the length of block to read
  BlkLen = BlkPos[CurBlk + 1] - BlkPos[CurBlk];

  if (trace(1))
    htrc("File position is now %d\n", ftell(Stream));

  // Read the entire next block
  n = fread(To_Buf, 1, (size_t)BlkLen, Stream);

  if ((size_t) n == (size_t) BlkLen) {
//  ReadBlks++;
    num_read++;
    Rbuf = (CurBlk == Block - 1) ? Last : Nrec;

   ok:
    rc = RC_OK;

    // Get the position of the current line
    for (i = 0, CurLine = To_Buf; i < CurNum; i++)
      while (*CurLine++ != '\n') ;      // What about Unix ???

    // Now get the position of the next line
    for (NxtLine = CurLine; *NxtLine++ != '\n';) ;

    // Set caller line buffer
    n = NxtLine - CurLine - Ending;
    memcpy(Tdbp->GetLine(), CurLine, n);
    Tdbp->GetLine()[n] = '\0';
  } else if (feof(Stream)) {
    rc = RC_EF;
  } else {
#if defined(_WIN32)
    sprintf(g->Message, MSG(READ_ERROR), To_File, _strerror(NULL));
#else
    sprintf(g->Message, MSG(READ_ERROR), To_File, strerror(errno));
#endif

    if (trace(1))
      htrc("%s\n", g->Message);

    return RC_FX;
  } // endelse

  OldBlk = CurBlk;         // Last block actually read
  IsRead = true;           // Is read indeed

 fin:
  // Store the current record file position for Delete and Update
  Fpos = (int)(BlkPos[CurBlk] + CurLine - To_Buf);
  return rc;
  } // end of ReadBuffer

/***********************************************************************/
/*  WriteBuffer: File write routine for the blocked DOS access method. */
/*  Update is directly written back into the file,                     */
/*         with this (fast) method, record size cannot change.         */
/***********************************************************************/
int BLKFAM::WriteBuffer(PGLOBAL g)
  {
  if (Tdbp->GetMode() == MODE_INSERT) {
    /*******************************************************************/
    /*  In Insert mode, blocks are added sequentially to the file end. */
    /*******************************************************************/
    if (!Closing) {                    // Add line to the write buffer
      strcat(strcpy(CurLine, Tdbp->GetLine()), CrLf);

      if (++CurNum != Rbuf) {
        CurLine += strlen(CurLine);
        return RC_OK;                  // We write only full blocks
        } // endif CurNum

      } // endif Closing

    //  Now start the writing process.
    NxtLine = CurLine + strlen(CurLine);
    BlkLen = (int)(NxtLine - To_Buf);

    if (fwrite(To_Buf, 1, BlkLen, Stream) != (size_t)BlkLen) {
      sprintf(g->Message, MSG(FWRITE_ERROR), strerror(errno));
      Closing = true;      // To tell CloseDB about a Write error
      return RC_FX;
      } // endif size

    CurBlk++;
    CurNum = 0;
    CurLine = To_Buf;
  } else {
    /*******************************************************************/
    /*  Mode == MODE_UPDATE.                                           */
    /*******************************************************************/
    const char  *crlf;
    size_t len;
    int   curpos = ftell(Stream);
    bool   moved = true;

    // T_Stream is the temporary stream or the table file stream itself
    if (!T_Stream)
      if (UseTemp /*&& Tdbp->GetMode() == MODE_UPDATE*/) {
        if (OpenTempFile(g))
          return RC_FX;

      } else
        T_Stream = Stream;

    if (UseTemp) {
      /*****************************************************************/
      /*  We are using a temporary file. Before writing the updated    */
      /*  record, we must eventually copy all the intermediate records */
      /*  that have not been updated.                                  */
      /*****************************************************************/
      if (MoveIntermediateLines(g, &moved))
        return RC_FX;

      Spos = GetNextPos();                     // New start position

      // Prepare the output buffer
#if defined(_WIN32)
      crlf = "\r\n";
#else
      crlf = "\n";
#endif   // _WIN32
      strcat(strcpy(OutBuf, Tdbp->GetLine()), crlf);
      len = strlen(OutBuf);
    } else {
      if (fseek(Stream, Fpos, SEEK_SET)) {   // Fpos is last position
        sprintf(g->Message, MSG(FSETPOS_ERROR), 0);
        return RC_FX;
        } // endif fseek

      // Replace the line inside read buffer (length has not changed)
      memcpy(CurLine, Tdbp->GetLine(), strlen(Tdbp->GetLine()));
      OutBuf = CurLine;
      len = (size_t)(NxtLine - CurLine);
    } // endif UseTemp

    if (fwrite(OutBuf, 1, len, T_Stream) != (size_t)len) {
      sprintf(g->Message, MSG(FWRITE_ERROR), strerror(errno));
      return RC_FX;
      } // endif fwrite

    if (moved)
      if (fseek(Stream, curpos, SEEK_SET)) {
        sprintf(g->Message, MSG(FSEEK_ERROR), strerror(errno));
        return RC_FX;
        } // endif

  } // endif Mode

  return RC_OK;
  } // end of WriteBuffer

/***********************************************************************/
/*  Table file close routine for DOS access method.                    */
/***********************************************************************/
void BLKFAM::CloseTableFile(PGLOBAL g, bool abort)
  {
  int rc, wrc = RC_OK;

  Abort = abort;

  if (UseTemp && T_Stream) {
    if (Tdbp->GetMode() == MODE_UPDATE && !Abort) {
      // Copy eventually remaining lines
      bool b;

      fseek(Stream, 0, SEEK_END);
      Fpos = ftell(Stream);
      Abort = MoveIntermediateLines(g, &b) != RC_OK;
      } // endif Abort

    // Delete the old file and rename the new temp file.
    rc = RenameTempFile(g);    // Also close all files
  } else {
    // Closing is True if last Write was in error
    if (Tdbp->GetMode() == MODE_INSERT && CurNum && !Closing) {
      // Some more inserted lines remain to be written
      Rbuf = CurNum--;
      Closing = true;
      wrc = WriteBuffer(g);
    } else if (Modif && !Closing) {
      // Last updated block remains to be written
      Closing = true;
      wrc = ReadBuffer(g);
    } // endif's

    rc = PlugCloseFile(g, To_Fb);

    if (trace(1))
      htrc("BLK CloseTableFile: closing %s mode=%d wrc=%d rc=%d\n",
            To_File, Tdbp->GetMode(), wrc, rc);

  } // endif UseTemp

  Stream = NULL;           // So we can know whether table is open
  } // end of CloseTableFile

/***********************************************************************/
/*  Rewind routine for DOS access method.                              */
/*  Note: commenting out OldBlk = -1 has two advantages:               */
/*  1 - It forces fseek on  first block, thus suppressing the need to  */
/*      rewind the file, anyway unuseful when second pass if indexed.  */
/*  2 - It permit to avoid re-reading small tables having only 1 block.*/
/***********************************************************************/
void BLKFAM::Rewind(void)
  {
//rewind(Stream);        will be placed by fseek
  CurBlk = -1;
  CurNum = Rbuf;
//OldBlk = -1;     commented out in case we reuse last read block
//Rbuf = 0;        commented out in case we reuse last read block
  } // end of Rewind

/* --------------------------- Class BINFAM -------------------------- */

#if 0
/***********************************************************************/
/*  BIN GetFileLength: returns file size in number of bytes.           */
/***********************************************************************/
int BINFAM::GetFileLength(PGLOBAL g)
{
	int len;

	if (!Stream)
		len = TXTFAM::GetFileLength(g);
	else
		if ((len = _filelength(_fileno(Stream))) < 0)
			sprintf(g->Message, MSG(FILELEN_ERROR), "_filelength", To_File);

	xtrc(1, "File length=%d\n", len);
	return len;
} // end of GetFileLength

/***********************************************************************/
/*  Cardinality: returns table cardinality in number of rows.          */
/*  This function can be called with a null argument to test the       */
/*  availability of Cardinality implementation (1 yes, 0 no).          */
/***********************************************************************/
int BINFAM::Cardinality(PGLOBAL g)
{
	return (g) ? -1 : 0;
} // end of Cardinality

/***********************************************************************/
/*  OpenTableFile: Open a DOS/UNIX table file using C standard I/Os.   */
/***********************************************************************/
bool BINFAM::OpenTableFile(PGLOBAL g) {
	char    opmode[4], filename[_MAX_PATH];
	MODE    mode = Tdbp->GetMode();
	PDBUSER dbuserp = PlgGetUser(g);

	switch (mode) {
	case MODE_READ:
		strcpy(opmode, "rb");
		break;
	case MODE_WRITE:
		strcpy(opmode, "wb");
		break;
	default:
		sprintf(g->Message, MSG(BAD_OPEN_MODE), mode);
		return true;
	} // endswitch Mode

	// Now open the file stream
	PlugSetPath(filename, To_File, Tdbp->GetPath());

	if (!(Stream = PlugOpenFile(g, filename, opmode))) {
		if (trace(1))
			htrc("%s\n", g->Message);

		return (mode == MODE_READ && errno == ENOENT)
			? PushWarning(g, Tdbp) : true;
	} // endif Stream

	if (trace(1))
		htrc("File %s open Stream=%p mode=%s\n", filename, Stream, opmode);

	To_Fb = dbuserp->Openlist;     // Keep track of File block

	/*********************************************************************/
	/*  Allocate the line buffer.                                        */
	/*********************************************************************/
	return AllocateBuffer(g);
} // end of OpenTableFile
#endif // 0

/***********************************************************************/
/*  Allocate the line buffer. For mode Delete a bigger buffer has to   */
/*  be allocated because is it also used to move lines into the file.  */
/***********************************************************************/
bool BINFAM::AllocateBuffer(PGLOBAL g)
{
  MODE mode = Tdbp->GetMode();

  // Lrecl is Ok
  Buflen = Lrecl;

  // Buffer will be allocated separately
  if (mode == MODE_ANY) {
    xtrc(1, "SubAllocating a buffer of %d bytes\n", Buflen);
    To_Buf = (char*)PlugSubAlloc(g, NULL, Buflen);
  } else if (UseTemp || mode == MODE_DELETE) {
    // Have a big buffer to move lines
    Dbflen = Buflen * DOS_BUFF_LEN;
    DelBuf = PlugSubAlloc(g, NULL, Dbflen);
  } // endif mode

  return false;
#if 0
  MODE mode = Tdbp->GetMode();

  // Lrecl is Ok
  Dbflen = Buflen = Lrecl;

  if (trace(1))
    htrc("SubAllocating a buffer of %d bytes\n", Buflen);

  DelBuf = To_Buf = (char*)PlugSubAlloc(g, NULL, Buflen);
  return false;
#endif // 0
} // end of AllocateBuffer

#if 0
/***********************************************************************/
/*  GetRowID: return the RowID of last read record.                    */
/***********************************************************************/
int BINFAM::GetRowID(void) {
	return Rows;
} // end of GetRowID

/***********************************************************************/
/*  GetPos: return the position of last read record.                   */
/***********************************************************************/
int BINFAM::GetPos(void) {
	return Fpos;
} // end of GetPos

/***********************************************************************/
/*  GetNextPos: return the position of next record.                    */
/***********************************************************************/
int BINFAM::GetNextPos(void) {
	return ftell(Stream);
} // end of GetNextPos

/***********************************************************************/
/*  SetPos: Replace the table at the specified position.               */
/***********************************************************************/
bool BINFAM::SetPos(PGLOBAL g, int pos) {
	Fpos = pos;

	if (fseek(Stream, Fpos, SEEK_SET)) {
		sprintf(g->Message, MSG(FSETPOS_ERROR), Fpos);
		return true;
	} // endif

	Placed = true;
	return false;
} // end of SetPos

/***********************************************************************/
/*  Record file position in case of UPDATE or DELETE.                  */
/***********************************************************************/
bool BINFAM::RecordPos(PGLOBAL g) {
	if ((Fpos = ftell(Stream)) < 0) {
		sprintf(g->Message, MSG(FTELL_ERROR), 0, strerror(errno));
		//  strcat(g->Message, " (possible wrong ENDING option value)");
		return true;
	} // endif Fpos

	return false;
} // end of RecordPos
#endif // 0

/***********************************************************************/
/*  ReadBuffer: Read one line for a text file.                         */
/***********************************************************************/
int BINFAM::ReadBuffer(PGLOBAL g)
{
	int rc;

	if (!Stream)
		return RC_EF;

	xtrc(2, "ReadBuffer: Tdbp=%p To_Line=%p Placed=%d\n",
					Tdbp, Tdbp->GetLine(), Placed);

	if (!Placed) {
		/*******************************************************************/
		/*  Record file position in case of UPDATE or DELETE.              */
		/*******************************************************************/
		if (RecordPos(g))
			return RC_FX;

		CurBlk = (int)Rows++;
		xtrc(2, "ReadBuffer: CurBlk=%d\n", CurBlk);
	} else
		Placed = false;

	xtrc(2, " About to read: bstream=%p To_Buf=%p Buflen=%d Fpos=%d\n",
						Stream, To_Buf, Buflen, Fpos);

	// Read the prefix giving the row length
	if (!fread(&Recsize, sizeof(size_t), 1, Stream)) {
		if (!feof(Stream)) {
			strcpy(g->Message, "Error reading line prefix\n");
			return RC_FX;
		} else
			return RC_EF;

	} else if (Recsize > (unsigned)Buflen) {
		sprintf(g->Message, "Record too big (Recsize=%zd Buflen=%d)\n", Recsize, Buflen);
		return RC_FX;
	}	// endif Recsize

	if (fread(To_Buf, Recsize, 1, Stream)) {
		xtrc(2, " Read: To_Buf=%p Recsize=%zd\n", To_Buf, Recsize);
		num_read++;
		rc = RC_OK;
	} else if (feof(Stream)) {
		rc = RC_EF;
	} else {
#if defined(_WIN32)
		sprintf(g->Message, MSG(READ_ERROR), To_File, _strerror(NULL));
#else
		sprintf(g->Message, MSG(READ_ERROR), To_File, strerror(0));
#endif
		xtrc(2, "%s\n", g->Message);
		rc = RC_FX;
	} // endif's fread

	xtrc(2, "ReadBuffer: rc=%d\n", rc);
	IsRead = true;
	return rc;
} // end of ReadBuffer

/***********************************************************************/
/*  WriteBuffer: File write routine for BIN access method.             */
/***********************************************************************/
int BINFAM::WriteBuffer(PGLOBAL g)
{
	int   curpos = 0;
	bool  moved = true;

  // T_Stream is the temporary stream or the table file stream itself
  if (!T_Stream) {
    if (UseTemp && Tdbp->GetMode() == MODE_UPDATE) {
      if (OpenTempFile(g))
        return RC_FX;

    } else
      T_Stream = Stream;

  } // endif T_Stream

  if (Tdbp->GetMode() == MODE_UPDATE) {
    /*******************************************************************/
    /*  Here we simply rewrite a record on itself. There are two cases */
    /*  were another method should be used, a/ when Update apply to    */
    /*  the whole file, b/ when updating the last field of a variable  */
    /*  length file. The method could be to rewrite a new file, then   */
    /*  to erase the old one and rename the new updated file.          */
    /*******************************************************************/
    curpos = ftell(Stream);

    if (trace(1))
      htrc("Last : %d cur: %d\n", Fpos, curpos);

    if (UseTemp) {
      /*****************************************************************/
      /*  We are using a temporary file.                               */
      /*  Before writing the updated record, we must eventually copy   */
      /*  all the intermediate records that have not been updated.     */
      /*****************************************************************/
      if (MoveIntermediateLines(g, &moved))
        return RC_FX;

      Spos = curpos;                            // New start position
    } else
      // Update is directly written back into the file,
      //   with this (fast) method, record size cannot change.
      if (fseek(Stream, Fpos, SEEK_SET)) {
        sprintf(g->Message, MSG(FSETPOS_ERROR), 0);
        return RC_FX;
      } // endif

  } // endif mode

  /*********************************************************************/
	/*  Prepare writing the line.                                        */
	/*********************************************************************/
//memcpy(To_Buf, Tdbp->GetLine(), Recsize);

	/*********************************************************************/
	/*  Now start the writing process.                                   */
	/*********************************************************************/
	if (fwrite(&Recsize, sizeof(size_t), 1, T_Stream) != 1) {
		sprintf(g->Message, "Error %d writing prefix to %s",
			errno, To_File);
		return RC_FX;
	} else if (fwrite(To_Buf, Recsize, 1, T_Stream) != 1) {
		sprintf(g->Message, "Error %d writing %zd bytes to %s",
			errno, Recsize, To_File);
		return RC_FX;
	} // endif fwrite

  if (Tdbp->GetMode() == MODE_UPDATE && moved)
    if (fseek(Stream, curpos, SEEK_SET)) {
      sprintf(g->Message, MSG(FSEEK_ERROR), strerror(errno));
      return RC_FX;
    } // endif

  xtrc(1, "Binary write done\n");
	return RC_OK;
} // end of WriteBuffer

#if 0
/***********************************************************************/
/*  Data Base delete line routine for DOS and BLK access methods.      */
/***********************************************************************/
int DOSFAM::DeleteRecords(PGLOBAL g, int irc)
{
  bool moved;
  int  curpos = ftell(Stream);

  /*********************************************************************/
  /*  There is an alternative here:                                    */
  /*  1 - use a temporary file in which are copied all not deleted     */
  /*      lines, at the end the original file will be deleted and      */
  /*      the temporary file renamed to the original file name.        */
  /*  2 - directly move the not deleted lines inside the original      */
  /*      file, and at the end erase all trailing records.             */
  /*  This will be experimented.                                       */
  /*********************************************************************/
  if (trace(1))
    htrc(
      "DOS DeleteDB: rc=%d UseTemp=%d curpos=%d Fpos=%d Tpos=%d Spos=%d\n",
      irc, UseTemp, curpos, Fpos, Tpos, Spos);

  if (irc != RC_OK) {
    /*******************************************************************/
    /*  EOF: position Fpos at the end-of-file position.                */
    /*******************************************************************/
    fseek(Stream, 0, SEEK_END);
    Fpos = ftell(Stream);

    if (trace(1))
      htrc("Fpos placed at file end=%d\n", Fpos);

  } // endif irc

  if (Tpos == Spos) {
    /*******************************************************************/
    /*  First line to delete, Open temporary file.                     */
    /*******************************************************************/
    if (UseTemp) {
      if (OpenTempFile(g))
        return RC_FX;

    } else {
      /*****************************************************************/
      /*  Move of eventual preceding lines is not required here.       */
      /*  Set the target file as being the source file itself.         */
      /*  Set the future Tpos, and give Spos a value to block copying. */
      /*****************************************************************/
      T_Stream = Stream;
      Spos = Tpos = Fpos;
    } // endif UseTemp

  } // endif Tpos == Spos

    /*********************************************************************/
    /*  Move any intermediate lines.                                     */
    /*********************************************************************/
  if (MoveIntermediateLines(g, &moved))
    return RC_FX;

  if (irc == RC_OK) {
    /*******************************************************************/
    /*  Reposition the file pointer and set Spos.                      */
    /*******************************************************************/
    if (!UseTemp || moved)
      if (fseek(Stream, curpos, SEEK_SET)) {
        sprintf(g->Message, MSG(FSETPOS_ERROR), 0);
        return RC_FX;
      } // endif

    Spos = GetNextPos();                     // New start position

    if (trace(1))
      htrc("after: Tpos=%d Spos=%d\n", Tpos, Spos);

  } else {
    /*******************************************************************/
    /*  Last call after EOF has been reached.                          */
    /*  The UseTemp case is treated in CloseTableFile.                 */
    /*******************************************************************/
    if (!UseTemp & !Abort) {
      /*****************************************************************/
      /*  Because the chsize functionality is only accessible with a   */
      /*  system call we must close the file and reopen it with the    */
      /*  open function (_fopen for MS ??) this is still to be checked */
      /*  for compatibility with Text files and other OS's.            */
      /*****************************************************************/
      char filename[_MAX_PATH];
      int  h;                           // File handle, return code

      PlugSetPath(filename, To_File, Tdbp->GetPath());
      /*rc=*/ PlugCloseFile(g, To_Fb);

      if ((h= global_open(g, MSGID_OPEN_STRERROR, filename, O_WRONLY)) <= 0)
        return RC_FX;

      /*****************************************************************/
      /*  Remove extra records.                                        */
      /*****************************************************************/
#if defined(_WIN32)
      if (chsize(h, Tpos)) {
        sprintf(g->Message, MSG(CHSIZE_ERROR), strerror(errno));
        close(h);
        return RC_FX;
      } // endif
#else
      if (ftruncate(h, (off_t)Tpos)) {
        sprintf(g->Message, MSG(TRUNCATE_ERROR), strerror(errno));
        close(h);
        return RC_FX;
      } // endif
#endif

      close(h);

      if (trace(1))
        htrc("done, h=%d irc=%d\n", h, irc);

    } // endif !UseTemp

  } // endif irc

  return RC_OK;                                      // All is correct
} // end of DeleteRecords

/***********************************************************************/
/*  Table file close routine for DOS access method.                    */
/***********************************************************************/
void BINFAM::CloseTableFile(PGLOBAL g, bool abort)
{
  int rc;

  Abort = abort;
  rc = PlugCloseFile(g, To_Fb);
  xtrc(1, "BIN Close: closing %s rc=%d\n", To_File, rc);
  Stream = NULL;           // So we can know whether table is open
} // end of CloseTableFile

/***********************************************************************/
/*  Rewind routine for BIN access method.                              */
/***********************************************************************/
void BINFAM::Rewind(void)
{
	if (Stream)  // Can be NULL when making index on void table
		rewind(Stream);

	Rows = 0;
	OldBlk = CurBlk = -1;
} // end of Rewind
#endif // 0
