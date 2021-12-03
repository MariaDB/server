/*********** File AM Map C++ Program Source Code File (.CPP) ***********/
/* PROGRAM NAME: FILAMAP                                               */
/* -------------                                                       */
/*  Version 1.6                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2020    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the MAP file access method classes.               */
/*                                                                     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the System header files.              */
/***********************************************************************/
#include "my_global.h"
#if defined(_WIN32)
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif   // __BORLANDC__
//#include <windows.h>
#else    // !_WIN32
#if defined(UNIX)
#include <errno.h>
#include <unistd.h>
#else    // !UNIX
#include <io.h>
#endif  // !UNIX
#include <fcntl.h>
#endif  // !_WIN32

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  filamtxt.h  is header containing the file AM classes declarations. */
/*  Note: these files are included inside the include files below.     */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "osutil.h"
#include "maputil.h"
#include "filamap.h"
#include "tabdos.h"
#include "tabfmt.h"

/* --------------------------- Class MAPFAM -------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
MAPFAM::MAPFAM(PDOSDEF tdp) : TXTFAM(tdp)
  {
  Memory = NULL;
  Mempos = NULL;
  Tpos = NULL;
  Fpos = NULL;
  Spos = NULL;
  Top = NULL;
  } // end of MAPFAM standard constructor

MAPFAM::MAPFAM(PMAPFAM tmfp) : TXTFAM(tmfp)
  {
  Memory = tmfp->Memory;
  Mempos = tmfp->Mempos;
  Fpos = tmfp->Fpos;
  Spos = tmfp->Spos;
  Tpos = tmfp->Tpos;
  Top = tmfp->Top;
  } // end of MAPFAM copy constructor

/***********************************************************************/
/*  Reset: reset position values at the beginning of file.             */
/***********************************************************************/
void MAPFAM::Reset(void)
  {
  TXTFAM::Reset();
  Fpos = Tpos = Spos = NULL;
  }  // end of Reset

/***********************************************************************/
/*  MAP GetFileLength: returns file size in number of bytes.           */
/***********************************************************************/
int MAPFAM::GetFileLength(PGLOBAL g)
  {
  int len;

  len = (To_Fb && To_Fb->Count) ? To_Fb->Length : TXTFAM::GetFileLength(g);

  if (trace(1))
    htrc("Mapped file length=%d\n", len);

  return len;
  } // end of GetFileLength

/***********************************************************************/
/*  OpenTableFile: Open a DOS/UNIX table file as a mapped file.        */
/***********************************************************************/
bool MAPFAM::OpenTableFile(PGLOBAL g)
  {
  char    filename[_MAX_PATH];
  size_t  len;
  MODE    mode = Tdbp->GetMode();
  PFBLOCK fp;
  PDBUSER dbuserp = (PDBUSER)g->Activityp->Aptr;

#if defined(_DEBUG)
  // Insert mode is no more handled using file mapping
  assert(mode != MODE_INSERT);
#endif   // _DEBUG

  /*********************************************************************/
  /*  We used the file name relative to recorded datapath.             */
  /*********************************************************************/
  PlugSetPath(filename, To_File, Tdbp->GetPath());

  /*********************************************************************/
  /*  Under Win32 the whole file will be mapped so we can use it as    */
  /*  if it were entirely read into virtual memory.                    */
  /*  Firstly we check whether this file have been already mapped.     */
  /*********************************************************************/
  if (mode == MODE_READ) {
    for (fp = dbuserp->Openlist; fp; fp = fp->Next)
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
    Memory = fp->Memory;
    len = fp->Length;
  } else {
    /*******************************************************************/
    /*  If required, delete the whole file if no filtering is implied. */
    /*******************************************************************/
    bool   del;
    HANDLE hFile;
    MEMMAP mm;

    del = mode == MODE_DELETE && !Tdbp->GetNext();

    if (del)
      DelRows = Cardinality(g);

    /*******************************************************************/
    /*  Create the mapping file object.                                */
    /*******************************************************************/
    hFile = CreateFileMap(g, filename, &mm, mode, del);

    if (hFile == INVALID_HANDLE_VALUE) {
      DWORD rc = GetLastError();

      if (!(*g->Message))
        sprintf(g->Message, MSG(OPEN_MODE_ERROR),
                "map", (int) rc, filename);

      if (trace(1))
        htrc("CreateFileMap: %s\n", g->Message);

      return (mode == MODE_READ && rc == ENOENT)
        ? false : true;
//      ? PushWarning(g, Tdbp) : true; --> assert fails into MariaDB
      } // endif hFile

    /*******************************************************************/
    /*  Get the file size.                                             */
    /*******************************************************************/
		len = (size_t)mm.lenL;
		
		if (mm.lenH)
			len += ((size_t)mm.lenH * 0x000000001LL);

    Memory = (char *)mm.memory;

    if (!len) {              // Empty or deleted file
      CloseFileHandle(hFile);
      Tdbp->ResetSize();
      return false;
      } // endif len

    if (!Memory) {
      CloseFileHandle(hFile);
      sprintf(g->Message, MSG(MAP_VIEW_ERROR),
                          filename, GetLastError());
      return true;
      } // endif Memory

#if defined(_WIN32)
    if (mode != MODE_DELETE) {
#else   // !_WIN32
    if (mode == MODE_READ) {
#endif  // !_WIN32
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

  /*********************************************************************/
  /*  The pseudo "buffer" is here the entire file mapping view.        */
  /*********************************************************************/
  Fpos = Mempos = Memory;
  Top = Memory + len;

  if (trace(1))
    htrc("fp=%p count=%d MapView=%p len=%d Top=%p\n",
          fp, fp->Count, Memory, len, Top);

  return AllocateBuffer(g);          // Useful for DBF files
  } // end of OpenTableFile

/***********************************************************************/
/*  GetRowID: return the RowID of last read record.                    */
/***********************************************************************/
int MAPFAM::GetRowID(void)
  {
  return Rows;
  } // end of GetRowID

/***********************************************************************/
/*  GetPos: return the position of last read record.                   */
/***********************************************************************/
int MAPFAM::GetPos(void)
  {
  return (int)(Fpos - Memory);
  } // end of GetPos

/***********************************************************************/
/*  GetNextPos: return the position of next record.                    */
/***********************************************************************/
int MAPFAM::GetNextPos(void)
  {
  return (int)(Mempos - Memory);
  } // end of GetNextPos

/***********************************************************************/
/*  SetPos: Replace the table at the specified position.               */
/***********************************************************************/
bool MAPFAM::SetPos(PGLOBAL g, int pos)
  {
  Fpos = Mempos = Memory + pos;

  if (Mempos >= Top || Mempos < Memory) {
    strcpy(g->Message, MSG(INV_MAP_POS));
    return true;
    } // endif Mempos

  Placed = true;
  return false;
  } // end of SetPos

/***********************************************************************/
/*  Record file position in case of UPDATE or DELETE.                  */
/***********************************************************************/
bool MAPFAM::RecordPos(PGLOBAL)
  {
  Fpos = Mempos;
  return false;
  } // end of RecordPos

/***********************************************************************/
/*  Initialize Fpos and Mempos for indexed DELETE.                     */
/***********************************************************************/
int MAPFAM::InitDelete(PGLOBAL, int fpos, int spos)
  {
  Fpos = Memory + (ptrdiff_t)fpos;
  Mempos = Memory + (ptrdiff_t)spos;
  return RC_OK;
  } // end of InitDelete

/***********************************************************************/
/*  Skip one record in file.                                           */
/***********************************************************************/
int MAPFAM::SkipRecord(PGLOBAL g, bool header)
  {
  PDBUSER dup = (PDBUSER)g->Activityp->Aptr;

  // Skip this record
  while (*Mempos++ != '\n')					 		// What about Unix ???
		if (Mempos == Top)
      return RC_EF;

  // Update progress information
  dup->ProgCur = GetPos();

  if (header)
    Fpos = Tpos = Spos = Mempos;      // For Delete

  return RC_OK;
  } // end of SkipRecord

/***********************************************************************/
/*  ReadBuffer: Read one line for a mapped text file.                  */
/***********************************************************************/
int MAPFAM::ReadBuffer(PGLOBAL g)
  {
  int rc, len, n = 1;

  // Are we at the end of the memory
	if (Mempos >= Top) {
		if ((rc = GetNext(g)) != RC_OK)
			return rc;
		else if (Tdbp->GetAmType() == TYPE_AM_CSV && ((PTDBCSV)Tdbp)->Header)
			if ((rc = SkipRecord(g, true)) != RC_OK)
				return rc;

	}	// endif Mempos


  if (!Placed) {
    /*******************************************************************/
    /*  Record file position in case of UPDATE or DELETE.              */
    /*******************************************************************/
   next:
    Fpos = Mempos;
    CurBlk = (int)Rows++;

    /*******************************************************************/
    /*  Check whether optimization on ROWID                            */
    /*  can be done, as well as for join as for local filtering.       */
    /*******************************************************************/
    switch (Tdbp->TestBlock(g)) {
      case RC_EF:
				if ((rc = GetNext(g)) != RC_OK)
					return rc;

      /* falls through */
      case RC_NF:
        // Skip this record
        if ((rc = SkipRecord(g, false)) != RC_OK)
          return rc;

        goto next;
      } // endswitch rc

  } else
    Placed = false;

  // Immediately calculate next position (Used by DeleteDB)
  while (*Mempos++ != '\n')          // What about Unix ???
		if (Mempos == Top) {
			n = 0;
			break;
		}	// endif Mempos

  // Set caller line buffer
  len = (int)(Mempos - Fpos) - n;

  // Don't rely on ENDING setting
  if (len > 0 && *(Mempos - 2) == '\r')
    len--;             // Line ends by CRLF

  memcpy(Tdbp->GetLine(), Fpos, len);
  Tdbp->GetLine()[len] = '\0';
  return RC_OK;
  } // end of ReadBuffer

/***********************************************************************/
/*  WriteBuffer: File write routine for MAP access method.             */
/***********************************************************************/
int MAPFAM::WriteBuffer(PGLOBAL g __attribute__((unused)))
  {
#if defined(_DEBUG)
  // Insert mode is no more handled using file mapping
  if (Tdbp->GetMode() == MODE_INSERT) {
    strcpy(g->Message, MSG(NO_MAP_INSERT));
    return RC_FX;
    } // endif
#endif   // _DEBUG

  /*********************************************************************/
  /*  Copy the updated record back into the memory mapped file.        */
  /*********************************************************************/
  memcpy(Fpos, Tdbp->GetLine(), strlen(Tdbp->GetLine()));
  return RC_OK;
  } // end of WriteBuffer

/***********************************************************************/
/*  Data Base delete line routine for MAP (and FIX?) access methods.   */
/*  Lines between deleted lines are moved in the mapfile view.         */
/***********************************************************************/
int MAPFAM::DeleteRecords(PGLOBAL g, int irc)
  {
  int    n;

  if (trace(1))
    htrc("MAP DeleteDB: irc=%d mempos=%p tobuf=%p Tpos=%p Spos=%p\n",
         irc, Mempos, To_Buf, Tpos, Spos);

  if (irc != RC_OK) {
    /*******************************************************************/
    /*  EOF: position Fpos at the top of map position.                 */
    /*******************************************************************/
    Fpos = Top;

    if (trace(1))
      htrc("Fpos placed at file top=%p\n", Fpos);

    } // endif irc

  if (Tpos == Spos) {
    /*******************************************************************/
    /*  First line to delete. Move of eventual preceding lines is     */
    /*  not required here, just setting of future Spos and Tpos.       */
    /*******************************************************************/
    Tpos = Spos = Fpos;
  } else if ((n = (int)(Fpos - Spos)) > 0) {
    /*******************************************************************/
    /*  Non consecutive line to delete. Move intermediate lines.       */
    /*******************************************************************/
    memmove(Tpos, Spos, n);
    Tpos += n;

    if (trace(1))
      htrc("move %d bytes\n", n);

  } // endif n

  if (irc == RC_OK) {
    Spos = Mempos;                               // New start position

    if (trace(1))
      htrc("after: Tpos=%p Spos=%p\n", Tpos, Spos);

  } else if (To_Fb) {                 // Can be NULL for deleted files
    /*******************************************************************/
    /*  Last call after EOF has been reached.                          */
    /*  We must firstly Unmap the view and use the saved file handle   */
    /*  to put an EOF at the end of the copied part of the file.       */
    /*******************************************************************/
    PFBLOCK fp = To_Fb;

    CloseMemMap(fp->Memory, (size_t)fp->Length);
    fp->Count = 0;                             // Avoid doing it twice

    if (!Abort) {
      /*****************************************************************/
      /*  Remove extra records.                                        */
      /*****************************************************************/
      n = (int)(Tpos - Memory);

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

#else    // UNIX
      if (ftruncate(fp->Handle, (off_t)n)) {
        sprintf(g->Message, MSG(TRUNCATE_ERROR), strerror(errno));
        close(fp->Handle);
        return RC_FX;
        } // endif

#endif   // UNIX
    } // endif Abort

#if defined(_WIN32)
  CloseHandle(fp->Handle);
#else    // UNIX
  close(fp->Handle);
#endif   // UNIX
  } // endif irc

  return RC_OK;                                      // All is correct
  } // end of DeleteRecords

/***********************************************************************/
/*  Table file close routine for MAP access method.                    */
/***********************************************************************/
void MAPFAM::CloseTableFile(PGLOBAL g, bool)
  {
  PlugCloseFile(g, To_Fb);
//To_Fb = NULL;              // To get correct file size in Cardinality

  if (trace(1))
    htrc("MAP Close: closing %s count=%d\n",
         To_File, (To_Fb) ? To_Fb->Count : 0);

  } // end of CloseTableFile

/***********************************************************************/
/*  Rewind routine for MAP access method.                              */
/***********************************************************************/
void MAPFAM::Rewind(void)
  {
  Mempos = Memory;
  } // end of Rewind

/* --------------------------- Class MBKFAM -------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
MBKFAM::MBKFAM(PDOSDEF tdp) : MAPFAM(tdp)
  {
  Blocked = true;
  Block = tdp->GetBlock();
  Last = tdp->GetLast();
  Nrec = tdp->GetElemt();
  BlkPos = tdp->GetTo_Pos();
  CurNum = Nrec;
  } // end of MBKFAM standard constructor

/***********************************************************************/
/*  Reset: reset position values at the beginning of file.             */
/***********************************************************************/
void MBKFAM::Reset(void)
  {
  MAPFAM::Reset();
  CurNum = Nrec;              // To start by a new block
  }  // end of Reset

/***********************************************************************/
/*  Cardinality: returns table cardinality in number of rows.          */
/*  This function can be called with a null argument to test the       */
/*  availability of Cardinality implementation (1 yes, 0 no).          */
/***********************************************************************/
int MBKFAM::Cardinality(PGLOBAL g)
  {
  return (g) ? (int)((Block - 1) * Nrec + Last) : 1;
  } // end of Cardinality

/***********************************************************************/
/*  Skip one record in file.                                           */
/***********************************************************************/
int MBKFAM::SkipRecord(PGLOBAL, bool)
  {
  return RC_OK;
  } // end of SkipRecord

/***********************************************************************/
/*  GetRowID: return the RowID of last read record.                    */
/***********************************************************************/
int MBKFAM::GetRowID(void)
  {
  return CurNum + Nrec * CurBlk + 1;
  } // end of GetRowID

/***********************************************************************/
/*  ReadBuffer: Read one line for a mapped Fix file.                   */
/***********************************************************************/
int MBKFAM::ReadBuffer(PGLOBAL g)
  {
  int rc, len;

  /*********************************************************************/
  /*  Sequential block reading when Placed is not true.                */
  /*********************************************************************/
  if (Placed) {
    Placed = false;
  } else if (Mempos >= Top) {        // Are we at the end of the memory
		if ((rc = GetNext(g)) != RC_OK)
			return rc;

	} else if (++CurNum < Nrec) {
    Fpos = Mempos;
  } else {
    /*******************************************************************/
    /*  New block.                                                     */
    /*******************************************************************/
    CurNum = 0;

   next:
    if (++CurBlk >= Block)
			if ((rc = GetNext(g)) != RC_OK)
				return rc;

    /*******************************************************************/
    /*  Before reading a new block, check whether block optimization   */
    /*  can be done, as well as for join as for local filtering.       */
    /*******************************************************************/
    switch (Tdbp->TestBlock(g)) {
      case RC_EF:
				if ((rc = GetNext(g)) != RC_OK)
					return rc;

				break;
			case RC_NF:
        goto next;
      } // endswitch rc

    Fpos = Mempos = Memory + BlkPos[CurBlk];
  } // endif's

  // Immediately calculate next position (Used by DeleteDB)
	while (*Mempos++ != '\n')          // What about Unix ???
		if (Mempos == Top)
			break;

  // Set caller line buffer
  len = (int)(Mempos - Fpos) - Ending;
  memcpy(Tdbp->GetLine(), Fpos, len);
  Tdbp->GetLine()[len] = '\0';
  return RC_OK;
  } // end of ReadBuffer

/***********************************************************************/
/*  Rewind routine for FIX MAP access method.                          */
/***********************************************************************/
void MBKFAM::Rewind(void)
  {
  Mempos = Memory + Headlen;
  CurBlk = -1;
  CurNum = Nrec;
  } // end of Rewind

/* --------------------------- Class MPXFAM -------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
MPXFAM::MPXFAM(PDOSDEF tdp) : MBKFAM(tdp)
  {
  Blksize = tdp->GetBlksize();
  Padded = tdp->GetPadded();

  if (Padded && Blksize)
    Nrec = Blksize / Lrecl;
  else {
    Nrec = (tdp->GetElemt()) ? tdp->GetElemt() : DOS_BUFF_LEN;
    Blksize = Nrec * Lrecl;
    Padded = false;
    } // endelse

  CurNum = Nrec;
  } // end of MPXFAM standard constructor

#if 0                 // MBKFAM routine is correct
/***********************************************************************/
/*  GetRowID: return the RowID of last read record.                    */
/***********************************************************************/
int MPXFAM::GetRowID(void)
  {
  return (Mempos - Memory - Headlen) / Lrecl;
  } // end of GetRowID
#endif

/***********************************************************************/
/*  GetPos: return the position of last read record.                   */
/***********************************************************************/
int MPXFAM::GetPos(void)
  {
  return (CurNum + Nrec * CurBlk);          // Computed file index
  } // end of GetPos

/***********************************************************************/
/*  SetPos: Replace the table at the specified position.               */
/***********************************************************************/
bool MPXFAM::SetPos(PGLOBAL g, int pos)
  {
  if (pos < 0) {
    strcpy(g->Message, MSG(INV_REC_POS));
    return true;
    } // endif recpos

  CurBlk = pos / Nrec;
  CurNum = pos % Nrec;
  Fpos = Mempos = Memory + Headlen + pos * Lrecl;

  // Indicate the table position was externally set
  Placed = true;
  return false;
  } // end of SetPos

/***********************************************************************/
/*  Initialize CurBlk, CurNum, Mempos and Fpos for indexed DELETE.     */
/***********************************************************************/
int MPXFAM::InitDelete(PGLOBAL, int fpos, int)
  {
  Fpos = Memory + Headlen + (ptrdiff_t)fpos * Lrecl;
  Mempos = Fpos + Lrecl;
  return RC_OK;
  } // end of InitDelete

/***********************************************************************/
/*  ReadBuffer: Read one line for a mapped Fix file.                   */
/***********************************************************************/
int MPXFAM::ReadBuffer(PGLOBAL g)
  {
	int rc;

  /*********************************************************************/
  /*  Sequential block reading when Placed is not true.                */
  /*********************************************************************/
  if (Placed) {
    Placed = false;
  } else if (Mempos >= Top) {        // Are we at the end of the memory
		if ((rc = GetNext(g)) != RC_OK)
			return rc;

	} else if (++CurNum < Nrec) {
    Fpos = Mempos;
  } else {
    /*******************************************************************/
    /*  New block.                                                     */
    /*******************************************************************/
    CurNum = 0;

   next:
    if (++CurBlk >= Block)
			return GetNext(g);

    /*******************************************************************/
    /*  Before reading a new block, check whether block optimization   */
    /*  can be done, as well as for join as for local filtering.       */
    /*******************************************************************/
    switch (Tdbp->TestBlock(g)) {
      case RC_EF:
				if ((rc = GetNext(g)) != RC_OK)
					return rc;

				break;
			case RC_NF:
        goto next;
      } // endswitch rc

    Fpos = Mempos = Headlen + Memory + CurBlk * Blksize;
  } // endif's

  Tdbp->SetLine(Mempos);

  // Immediately calculate next position (Used by DeleteDB)
  Mempos += Lrecl;
  return RC_OK;
  } // end of ReadBuffer

/***********************************************************************/
/*  WriteBuffer: File write routine for MAP access method.             */
/***********************************************************************/
int MPXFAM::WriteBuffer(PGLOBAL g __attribute__((unused)))
  {
#if defined(_DEBUG)
  // Insert mode is no more handled using file mapping
  if (Tdbp->GetMode() == MODE_INSERT) {
    strcpy(g->Message, MSG(NO_MAP_INSERT));
    return RC_FX;
    } // endif
#endif   // _DEBUG

  // In Update mode, file was modified in memory
  return RC_OK;
  } // end of WriteBuffer

