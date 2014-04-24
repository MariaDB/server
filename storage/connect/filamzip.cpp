/*********** File AM Zip C++ Program Source Code File (.CPP) ***********/
/* PROGRAM NAME: FILAMZIP                                              */
/* -------------                                                       */
/*  Version 1.4                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2013    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the ZLIB compressed files classes.                */
/*                                                                     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                  */
/***********************************************************************/
#include "my_global.h"
#if defined(WIN32)
#include <io.h>
#include <fcntl.h>
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif
//#include <windows.h>
#else   // !WIN32
#if defined(UNIX)
#include <errno.h>
#else   // !UNIX
#include <io.h>
#endif
#include <fcntl.h>
#endif  // !WIN32

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  tabdos.h    is header containing the TABDOS class declarations.    */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
//#include "catalog.h"
//#include "reldef.h"
//#include "xobject.h"
//#include "kindex.h"
#include "filamtxt.h"
#include "tabdos.h"
#if defined(UNIX)
#include "osutil.h"
#endif

/***********************************************************************/
/*  This define prepares ZLIB function declarations.                   */
/***********************************************************************/
//#define ZLIB_DLL

#include "filamzip.h"

/***********************************************************************/
/*  DB static variables.                                               */
/***********************************************************************/
extern int num_read, num_there, num_eq[];                 // Statistics
extern "C" int  trace;

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Implementation of the ZIPFAM class.                                */
/***********************************************************************/
ZIPFAM::ZIPFAM(PZIPFAM txfp) : TXTFAM(txfp)
  {
  Zfile = txfp->Zfile;
  Zpos = txfp->Zpos;
  } // end of ZIPFAM copy constructor

/***********************************************************************/
/*  Zerror: Error function for gz calls.                               */
/*  gzerror returns the error message for the last error which occurred*/
/*  on the given compressed file. errnum is set to zlib error number.  */
/*  If an error occurred in the file system and not in the compression */
/*  library, errnum is set to Z_ERRNO and the application may consult  */
/*  errno to get the exact error code.                                 */
/***********************************************************************/
int ZIPFAM::Zerror(PGLOBAL g)
  {
  int errnum;

  strcpy(g->Message, gzerror(Zfile, &errnum));

  if (errnum == Z_ERRNO)
#if defined(WIN32)
    sprintf(g->Message, MSG(READ_ERROR), To_File, strerror(NULL));
#else   // !WIN32
    sprintf(g->Message, MSG(READ_ERROR), To_File, strerror(errno));
#endif  // !WIN32

    return (errnum == Z_STREAM_END) ? RC_EF : RC_FX;
  } // end of Zerror

/***********************************************************************/
/*  Reset: reset position values at the beginning of file.             */
/***********************************************************************/
void ZIPFAM::Reset(void)
  {
  TXTFAM::Reset();
//gzrewind(Zfile);                  // Useful ?????
  Zpos = 0;
  } // end of Reset

/***********************************************************************/
/*  ZIP GetFileLength: returns an estimate of what would be the        */
/*  uncompressed file size in number of bytes.                         */
/***********************************************************************/
int ZIPFAM::GetFileLength(PGLOBAL g)
  {
  int len = TXTFAM::GetFileLength(g);

  if (len > 0)
    // Estimate size reduction to a max of 6
    len *= 6;

  return len;
  } // end of GetFileLength

/***********************************************************************/
/*  ZIP Access Method opening routine.                                 */
/***********************************************************************/
bool ZIPFAM::OpenTableFile(PGLOBAL g)
  {
  char    opmode[4], filename[_MAX_PATH];
  MODE    mode = Tdbp->GetMode();

  switch (mode) {
    case MODE_READ:
      strcpy(opmode, "r");
      break;
    case MODE_UPDATE:
      /*****************************************************************/
      /* Updating ZIP files not implemented yet.                       */
      /*****************************************************************/
      strcpy(g->Message, MSG(UPD_ZIP_NOT_IMP));
      return true;
    case MODE_DELETE:
      if (!Tdbp->GetNext()) {
        // Store the number of deleted lines
        DelRows = Cardinality(g);

        // This will erase the entire file
        strcpy(opmode, "w");
//      Block = 0;                // For ZBKFAM
//      Last = Nrec;              // For ZBKFAM
        Tdbp->ResetSize();
      } else {
        sprintf(g->Message, MSG(NO_PART_DEL), "ZIP");
        return true;
      } // endif filter

      break;
    case MODE_INSERT:
      strcpy(opmode, "a+");
      break;
    default:
      sprintf(g->Message, MSG(BAD_OPEN_MODE), mode);
      return true;
    } // endswitch Mode

  /*********************************************************************/
  /*  Open according to logical input/output mode required.            */
  /*  Use specific zlib functions.                                     */
  /*  Treat files as binary.                                           */
  /*********************************************************************/
  strcat(opmode, "b");
  Zfile = gzopen(PlugSetPath(filename, To_File, Tdbp->GetPath()), opmode);

  if (Zfile == NULL) {
    sprintf(g->Message, MSG(GZOPEN_ERROR),
            opmode, (int)errno, filename);
    strcat(strcat(g->Message, ": "), strerror(errno));
    return (mode == MODE_READ && errno == ENOENT)
            ? PushWarning(g, Tdbp) : true;
    } // endif Zfile

  /*********************************************************************/
  /*  Something to be done here. >>>>>>>> NOT DONE <<<<<<<<            */
  /*********************************************************************/
//To_Fb = dbuserp->Openlist;     // Keep track of File block

  /*********************************************************************/
  /*  Allocate the line buffer.                                        */
  /*********************************************************************/
  return AllocateBuffer(g);
  } // end of OpenTableFile

/***********************************************************************/
/*  Allocate the line buffer. For mode Delete a bigger buffer has to   */
/*  be allocated because is it also used to move lines into the file.  */
/***********************************************************************/
bool ZIPFAM::AllocateBuffer(PGLOBAL g)
  {
  MODE mode = Tdbp->GetMode();

  Buflen = Lrecl + 2;                     // Lrecl does not include CRLF
//Buflen *= ((Mode == MODE_DELETE) ? DOS_BUFF_LEN : 1);    NIY

  if (trace)
    htrc("SubAllocating a buffer of %d bytes\n", Buflen);

  To_Buf = (char*)PlugSubAlloc(g, NULL, Buflen);

  if (mode == MODE_INSERT) {
    /*******************************************************************/
    /*  For Insert buffer must be prepared.                            */
    /*******************************************************************/
    memset(To_Buf, ' ', Buflen);
    To_Buf[Buflen - 2] = '\n';
    To_Buf[Buflen - 1] = '\0';
    } // endif Insert

  return false;
  } // end of AllocateBuffer

/***********************************************************************/
/*  GetRowID: return the RowID of last read record.                    */
/***********************************************************************/
int ZIPFAM::GetRowID(void)
  {
  return Rows;
  } // end of GetRowID

/***********************************************************************/
/*  GetPos: return the position of last read record.                   */
/***********************************************************************/
int ZIPFAM::GetPos(void)
  {
  return (int)Zpos;
  } // end of GetPos

/***********************************************************************/
/*  GetNextPos: return the position of next record.                    */
/***********************************************************************/
int ZIPFAM::GetNextPos(void)
  {
  return gztell(Zfile);
  } // end of GetNextPos

/***********************************************************************/
/*  SetPos: Replace the table at the specified position.               */
/***********************************************************************/
bool ZIPFAM::SetPos(PGLOBAL g, int pos)
  {
  sprintf(g->Message, MSG(NO_SETPOS_YET), "ZIP");
  return true;
#if 0
  Fpos = pos;

  if (fseek(Stream, Fpos, SEEK_SET)) {
    sprintf(g->Message, MSG(FSETPOS_ERROR), Fpos);
    return true;
    } // endif

  Placed = true;
  return false;
#endif // 0
  } // end of SetPos

/***********************************************************************/
/*  Record file position in case of UPDATE or DELETE.                  */
/***********************************************************************/
bool ZIPFAM::RecordPos(PGLOBAL g)
  {
  Zpos = gztell(Zfile);
  return false;
  } // end of RecordPos

/***********************************************************************/
/*  Skip one record in file.                                           */
/***********************************************************************/
int ZIPFAM::SkipRecord(PGLOBAL g, bool header)
  {
  // Skip this record
  if (gzeof(Zfile))
    return RC_EF;
  else if (gzgets(Zfile, To_Buf, Buflen) == Z_NULL)
    return Zerror(g);

  if (header)
    RecordPos(g);

  return RC_OK;
  } // end of SkipRecord

/***********************************************************************/
/*  ReadBuffer: Read one line from a compressed text file.             */
/***********************************************************************/
int ZIPFAM::ReadBuffer(PGLOBAL g)
  {
  char *p;
  int   rc;

  if (!Zfile)
    return RC_EF;

  if (!Placed) {
    /*******************************************************************/
    /*  Record file position in case of UPDATE or DELETE.              */
    /*******************************************************************/
    if (RecordPos(g))
      return RC_FX;

    CurBlk = Rows++;                        // Update RowID
  } else
    Placed = false;

  if (gzeof(Zfile)) {
    rc = RC_EF;
  } else if (gzgets(Zfile, To_Buf, Buflen) != Z_NULL) {
    p = To_Buf + strlen(To_Buf) - 1;

    if (*p == '\n')
      *p = '\0';              // Eliminate ending new-line character

    if (*(--p) == '\r')
      *p = '\0';              // Eliminate eventuel carriage return

    strcpy(Tdbp->GetLine(), To_Buf);
    IsRead = true;
    rc = RC_OK;
    num_read++;
  } else
    rc = Zerror(g);

  if (trace > 1)
    htrc(" Read: '%s' rc=%d\n", To_Buf, rc);

  return rc;
  } // end of ReadBuffer

/***********************************************************************/
/*  WriteDB: Data Base write routine for ZDOS access method.           */
/*  Update is not possible without using a temporary file (NIY).       */
/***********************************************************************/
int ZIPFAM::WriteBuffer(PGLOBAL g)
  {
  /*********************************************************************/
  /*  Prepare the write buffer.                                        */
  /*********************************************************************/
  strcat(strcpy(To_Buf, Tdbp->GetLine()), CrLf);

  /*********************************************************************/
  /*  Now start the writing process.                                   */
  /*********************************************************************/
  if (gzputs(Zfile, To_Buf) < 0)
    return Zerror(g);

  return RC_OK;
  } // end of WriteBuffer

/***********************************************************************/
/*  Data Base delete line routine for ZDOS access method.  (NIY)       */
/***********************************************************************/
int ZIPFAM::DeleteRecords(PGLOBAL g, int irc)
  {
  strcpy(g->Message, MSG(NO_ZIP_DELETE));
  return RC_FX;
  } // end of DeleteRecords

/***********************************************************************/
/*  Data Base close routine for DOS access method.                     */
/***********************************************************************/
void ZIPFAM::CloseTableFile(PGLOBAL g)
  {
  int rc = gzclose(Zfile);

  if (trace)
    htrc("ZIP CloseDB: closing %s rc=%d\n", To_File, rc);

  Zfile = NULL;            // So we can know whether table is open
//To_Fb->Count = 0;        // Avoid double closing by PlugCloseAll
  } // end of CloseTableFile

/***********************************************************************/
/*  Rewind routine for ZIP access method.                              */
/***********************************************************************/
void ZIPFAM::Rewind(void)
  {
  gzrewind(Zfile);
  } // end of Rewind

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
ZBKFAM::ZBKFAM(PDOSDEF tdp) : ZIPFAM(tdp)
  {
  Blocked = true;
  Block = tdp->GetBlock();
  Last = tdp->GetLast();
  Nrec = tdp->GetElemt();
  CurLine = NULL;
  NxtLine = NULL;
  Closing = false;
  BlkPos = NULL;
  } // end of ZBKFAM standard constructor

ZBKFAM::ZBKFAM(PZBKFAM txfp) : ZIPFAM(txfp)
  {
  CurLine = txfp->CurLine;
  NxtLine = txfp->NxtLine;
  Closing = txfp->Closing;
  } // end of ZBKFAM copy constructor

/***********************************************************************/
/*  ZBK Cardinality: returns table cardinality in number of rows.      */
/*  This function can be called with a null argument to test the       */
/*  availability of Cardinality implementation (1 yes, 0 no).          */
/***********************************************************************/
int ZBKFAM::Cardinality(PGLOBAL g)
  {
  // Should not be called in this version
  return (g) ? -1 : 0;
//return (g) ? (int)((Block - 1) * Nrec + Last) : 1;
  } // end of Cardinality

/***********************************************************************/
/*  Allocate the line buffer. For mode Delete a bigger buffer has to   */
/*  be allocated because is it also used to move lines into the file.  */
/***********************************************************************/
bool ZBKFAM::AllocateBuffer(PGLOBAL g)
  {
  Buflen = Nrec * (Lrecl + 2);
  CurLine = To_Buf = (char*)PlugSubAlloc(g, NULL, Buflen);

  if (Tdbp->GetMode() == MODE_INSERT) {
    // Set values so Block and Last can be recalculated
    if (Last == Nrec) {
      CurBlk = Block;
      Rbuf = Nrec;                   // To be used by WriteDB
    } else {
      // The last block must be completed
      CurBlk = Block - 1;
      Rbuf = Nrec - Last;            // To be used by WriteDB
    } // endif Last

    } // endif Insert

  return false;
  } // end of AllocateBuffer

/***********************************************************************/
/*  GetRowID: return the RowID of last read record.                    */
/***********************************************************************/
int ZBKFAM::GetRowID(void)
  {
  return CurNum + Nrec * CurBlk + 1;
  } // end of GetRowID

/***********************************************************************/
/*  GetPos: return the position of last read record.                   */
/***********************************************************************/
int ZBKFAM::GetPos(void)
  {
  return CurNum + Nrec * CurBlk;            // Computed file index
  } // end of GetPos

/***********************************************************************/
/*  Record file position in case of UPDATE or DELETE.                  */
/*  Not used yet for fixed tables.                                     */
/***********************************************************************/
bool ZBKFAM::RecordPos(PGLOBAL g)
  {
//strcpy(g->Message, "RecordPos not implemented for zip blocked tables");
//return true;
  return RC_OK;
  } // end of RecordPos

/***********************************************************************/
/*  Skip one record in file.                                           */
/***********************************************************************/
int ZBKFAM::SkipRecord(PGLOBAL g, bool header)
  {
//strcpy(g->Message, "SkipRecord not implemented for zip blocked tables");
//return RC_FX;
  return RC_OK;
  } // end of SkipRecord

/***********************************************************************/
/*  ReadBuffer: Read one line from a compressed text file.             */
/***********************************************************************/
int ZBKFAM::ReadBuffer(PGLOBAL g)
  {
  strcpy(g->Message, "This AM cannot be used in this version");
  return RC_FX;
  } // end of ReadBuffer

/***********************************************************************/
/*  WriteDB: Data Base write routine for ZDOS access method.           */
/*  Update is not possible without using a temporary file (NIY).       */
/***********************************************************************/
int ZBKFAM::WriteBuffer(PGLOBAL g)
  {
  /*********************************************************************/
  /*  Prepare the write buffer.                                        */
  /*********************************************************************/
  if (!Closing)
    strcat(strcpy(CurLine, Tdbp->GetLine()), CrLf);

  /*********************************************************************/
  /*  In Insert mode, blocs are added sequentialy to the file end.     */
  /*  Note: Update mode is not handled for zip files.                  */
  /*********************************************************************/
  if (++CurNum == Rbuf) {
    /*******************************************************************/
    /*  New block, start the writing process.                          */
    /*******************************************************************/
    BlkLen = CurLine + strlen(CurLine) - To_Buf;

    if (gzwrite(Zfile, To_Buf, BlkLen) != BlkLen ||
        gzflush(Zfile, Z_FULL_FLUSH)) {
      Closing = true;
      return Zerror(g);
      } // endif gzwrite

    Rbuf = Nrec;
    CurBlk++;
    CurNum = 0;
    CurLine = To_Buf;
  } else
    CurLine += strlen(CurLine);

  return RC_OK;
  } // end of WriteBuffer

/***********************************************************************/
/*  Data Base delete line routine for ZBK access method.               */
/*  Implemented only for total deletion of the table, which is done    */
/*  by opening the file in mode "wb".                                  */
/***********************************************************************/
int ZBKFAM::DeleteRecords(PGLOBAL g, int irc)
  {
  if (irc == RC_EF) {
    LPCSTR  name = Tdbp->GetName();
    PDOSDEF defp = (PDOSDEF)Tdbp->GetDef();

    defp->SetBlock(0);
    defp->SetLast(Nrec);

    if (!defp->SetIntCatInfo("Blocks", 0) ||
        !defp->SetIntCatInfo("Last", 0)) {
      sprintf(g->Message, MSG(UPDATE_ERROR), "Header");
      return RC_FX;
    } else
      return RC_OK;

  } else
    return irc;

  } // end of DeleteRecords

/***********************************************************************/
/*  Data Base close routine for ZBK access method.                     */
/***********************************************************************/
void ZBKFAM::CloseTableFile(PGLOBAL g)
  {
  int rc = RC_OK;

  if (Tdbp->GetMode() == MODE_INSERT) {
    LPCSTR  name = Tdbp->GetName();
    PDOSDEF defp = (PDOSDEF)Tdbp->GetDef();

    if (CurNum && !Closing) {
      // Some more inserted lines remain to be written
      Last = (Nrec - Rbuf) + CurNum;
      Block = CurBlk + 1;
      Rbuf = CurNum--;
      Closing = true;
      rc = WriteBuffer(g);
    } else if (Rbuf == Nrec) {
      Last = Nrec;
      Block = CurBlk;
    } // endif CurNum

    if (rc != RC_FX) {
      defp->SetBlock(Block);
      defp->SetLast(Last);
      defp->SetIntCatInfo("Blocks", Block);
      defp->SetIntCatInfo("Last", Last);
      } // endif

    gzclose(Zfile);
  } else if (Tdbp->GetMode() == MODE_DELETE) {
    rc = DeleteRecords(g, RC_EF);
    gzclose(Zfile);
  } else
    rc = gzclose(Zfile);

  if (trace)
    htrc("ZIP CloseDB: closing %s rc=%d\n", To_File, rc);

  Zfile = NULL;            // So we can know whether table is open
//To_Fb->Count = 0;        // Avoid double closing by PlugCloseAll
  } // end of CloseTableFile

/***********************************************************************/
/*  Rewind routine for ZBK access method.                              */
/***********************************************************************/
void ZBKFAM::Rewind(void)
  {
  gzrewind(Zfile);
  CurBlk = -1;
  CurNum = Rbuf;
  } // end of Rewind

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
ZIXFAM::ZIXFAM(PDOSDEF tdp) : ZBKFAM(tdp)
  {
//Block = tdp->GetBlock();
//Last = tdp->GetLast();
  Nrec = (tdp->GetElemt()) ? tdp->GetElemt() : DOS_BUFF_LEN;
  Blksize = Nrec * Lrecl;
  } // end of ZIXFAM standard constructor

/***********************************************************************/
/*  ZIX Cardinality: returns table cardinality in number of rows.      */
/*  This function can be called with a null argument to test the       */
/*  availability of Cardinality implementation (1 yes, 0 no).          */
/***********************************************************************/
int ZIXFAM::Cardinality(PGLOBAL g)
  {
  if (Last)
    return (g) ? (int)((Block - 1) * Nrec + Last) : 1;
  else  // Last and Block not defined, cannot do it yet
    return 0;

  } // end of Cardinality

/***********************************************************************/
/*  Allocate the line buffer. For mode Delete a bigger buffer has to   */
/*  be allocated because is it also used to move lines into the file.  */
/***********************************************************************/
bool ZIXFAM::AllocateBuffer(PGLOBAL g)
  {
  Buflen = Blksize;
  To_Buf = (char*)PlugSubAlloc(g, NULL, Buflen);

  if (Tdbp->GetMode() == MODE_INSERT) {
    /*******************************************************************/
    /*  For Insert the buffer must be prepared.                        */
    /*******************************************************************/
    memset(To_Buf, ' ', Buflen);

    if (Tdbp->GetFtype() < 2)
      // if not binary, the file is physically a text file
      for (int len = Lrecl; len <= Buflen; len += Lrecl) {
#if defined(WIN32)
        To_Buf[len - 2] = '\r';
#endif   // WIN32
        To_Buf[len - 1] = '\n';
        } // endfor len

    // Set values so Block and Last can be recalculated
    if (Last == Nrec) {
      CurBlk = Block;
      Rbuf = Nrec;                   // To be used by WriteDB
    } else {
      // The last block must be completed
      CurBlk = Block - 1;
      Rbuf = Nrec - Last;            // To be used by WriteDB
    } // endif Last

    } // endif Insert

  return false;
  } // end of AllocateBuffer

/***********************************************************************/
/*  ReadBuffer: Read one line from a compressed text file.             */
/***********************************************************************/
int ZIXFAM::ReadBuffer(PGLOBAL g)
  {
  int n, rc = RC_OK;

  /*********************************************************************/
  /*  Sequential reading when Placed is not true.                      */
  /*********************************************************************/
  if (++CurNum < Rbuf) {
    Tdbp->IncLine(Lrecl);                // Used by DOSCOL functions
    return RC_OK;
  } else if (Rbuf < Nrec && CurBlk != -1)
    return RC_EF;

  /*********************************************************************/
  /*  New block.                                                       */
  /*********************************************************************/
  CurNum = 0;
  Tdbp->SetLine(To_Buf);

  if (!(n = gzread(Zfile, To_Buf, Buflen))) {
    rc = RC_EF;
  } else if (n > 0) {
    Rbuf = n / Lrecl;
    IsRead = true;
    rc = RC_OK;
    num_read++;
  } else
    rc = Zerror(g);

  return rc;
  } // end of ReadBuffer

/***********************************************************************/
/*  WriteDB: Data Base write routine for ZDOS access method.           */
/*  Update is not possible without using a temporary file (NIY).       */
/***********************************************************************/
int ZIXFAM::WriteBuffer(PGLOBAL g)
  {
  /*********************************************************************/
  /*  In Insert mode, blocs are added sequentialy to the file end.     */
  /*  Note: Update mode is not handled for zip files.                  */
  /*********************************************************************/
  if (++CurNum == Rbuf) {
    /*******************************************************************/
    /*  New block, start the writing process.                          */
    /*******************************************************************/
    BlkLen = Rbuf * Lrecl;

    if (gzwrite(Zfile, To_Buf, BlkLen) != BlkLen ||
        gzflush(Zfile, Z_FULL_FLUSH)) {
      Closing = true;
      return Zerror(g);
      } // endif gzwrite

    Rbuf = Nrec;
    CurBlk++;
    CurNum = 0;
    Tdbp->SetLine(To_Buf);
  } else
    Tdbp->IncLine(Lrecl);            // Used by FIXCOL functions

  return RC_OK;
  } // end of WriteBuffer

/* ------------------------ End of ZipFam ---------------------------- */
