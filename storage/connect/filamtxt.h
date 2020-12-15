/************** FilAMTxt H Declares Source Code File (.H) **************/
/*  Name: FILAMTXT.H    Version 1.4                                    */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2020    */
/*                                                                     */
/*  This file contains the file access method classes declares.        */
/***********************************************************************/

#ifndef __FILAMTXT_H
#define __FILAMTXT_H

#include "block.h"
#include "array.h"

typedef class TXTFAM *PTXF;
typedef class DOSFAM *PDOSFAM;
typedef class BLKFAM *PBLKFAM;
typedef class BINFAM *PBINFAM;
typedef class DOSDEF *PDOSDEF;
typedef class TDBDOS *PTDBDOS;

/***********************************************************************/
/*  This is the base class for all file access method classes.         */
/***********************************************************************/
class DllExport TXTFAM : public BLOCK {
  friend class TDBDOS;
  friend class TDBCSV;
  friend class TDBFIX;
  friend class TDBVCT;
  friend class TDBJSON;
  friend class DOSCOL;
  friend class BINCOL;
  friend class VCTCOL;
 public:
  // Constructor
  TXTFAM(PDOSDEF tdp);
  TXTFAM(PTXF txfp);

  // Implementation
  virtual AMT   GetAmType(void) = 0;
  virtual int   GetPos(void) = 0;
  virtual int   GetNextPos(void) = 0;
  virtual PTXF  Duplicate(PGLOBAL g) = 0;
  virtual bool  GetUseTemp(void) {return false;}
  virtual int   GetDelRows(void) {return DelRows;}
        PFBLOCK GetTo_Fb(void) {return To_Fb;}
          int   GetCurBlk(void) {return CurBlk;}
          void  SetTdbp(PTDBDOS tdbp) {Tdbp = tdbp;}
          int   GetBlock(void) {return Block;}
          void  SetBlkPos(int *bkp) {BlkPos = bkp;}
          void  SetNrec(int n) {Nrec = n;}
          char *GetBuf(void) {return To_Buf;}
          int   GetRows(void) {return Rows;}
          bool  IsBlocked(void) {return Blocked;}

  // Methods
  virtual void  Reset(void);
  virtual int   GetFileLength(PGLOBAL g);
  virtual int   Cardinality(PGLOBAL g);
  virtual int   MaxBlkSize(PGLOBAL g, int s);
  virtual bool  AllocateBuffer(PGLOBAL) {return false;}
  virtual void  ResetBuffer(PGLOBAL) {}
  virtual int   GetNerr(void) {return 0;}
  virtual int   GetRowID(void) = 0;
  virtual bool  RecordPos(PGLOBAL g) = 0;
  virtual bool  SetPos(PGLOBAL g, int recpos) = 0;
  virtual int   SkipRecord(PGLOBAL g, bool header) = 0;
  virtual bool  OpenTableFile(PGLOBAL g) = 0;
  virtual bool  DeferReading(void) {IsRead = false; return true;}
  virtual int   ReadBuffer(PGLOBAL g) = 0;
  virtual int   WriteBuffer(PGLOBAL g) = 0;
  virtual int   DeleteRecords(PGLOBAL g, int irc) = 0;
  virtual void  CloseTableFile(PGLOBAL g, bool abort) = 0;
  virtual void  Rewind(void) = 0;
  virtual int   InitDelete(PGLOBAL g, int fpos, int spos);
          bool  AddListValue(PGLOBAL g, int type, void *val, PPARM *top);
          int   StoreValues(PGLOBAL g, bool upd);
          int   UpdateSortedRows(PGLOBAL g);
          int   DeleteSortedRows(PGLOBAL g);

 protected:
  // Members
  PTDBDOS Tdbp;              // To table class
  PCSZ    To_File;           // Points to table file name
  PFBLOCK To_Fb;             // Pointer to file block
  PPARM   To_Pos;            // Pointer to position list
  PPARM   To_Sos;            // Pointer to start position list
  PPARM   To_Upd;            // Pointer to udated line list
  PARRAY  Posar;             // Pointer to position array
  PARRAY  Sosar;             // Pointer to start position array
  PARRAY  Updar;             // Pointer to udated lines array
  bool    Placed;            // true if Recpos was externally set
  bool    IsRead;            // false for deferred reading
  bool    Blocked;           // true if using blocked I/O
  char   *To_Buf;            // Points to I/O buffer
  void   *DelBuf;            // Buffer used to move lines in Delete
  int    *BlkPos;            // To array of block positions
  int     BlkLen;            // Current block length
  int     Buflen;            // Buffer length
  int     Dbflen;            // Delete buffer length
  int     Rows;              // Number of rows read so far
  int     DelRows;           // Number of deleted rows
  int     Headlen;           // Number of bytes in header
  int     Lrecl;             // Logical Record Length
  int     Block;             // Number of blocks in table
  int     Last;              // Number of elements of last block
  int     Nrec;              // Number of records in buffer
  int     OldBlk;            // Index of last read block
  int     CurBlk;            // Index of current block
  int     CurNum;            // Current buffer line number
  int     ReadBlks;          // Number of blocks read (selected)
  int     Rbuf;              // Number of lines read in buffer
  int     Modif;             // Number of modified lines in block
  int     Blksize;           // Size of padded blocks
  int     Ending;            // Length of line end
  int     Fpos;              // Position of last read record
  int     Spos;              // Start position for update/delete move
  int     Tpos;              // Target Position for delete move
  bool    Padded;            // true if fixed size blocks are padded
  bool    Eof;               // true if an EOF (0xA) character exists
  bool    Abort;             // To abort on error
  char   *CrLf;              // End of line character(s)
  }; // end of class TXTFAM

/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for standard  */
/*  text files with variable record format (DOS, CSV, FMT)             */
/***********************************************************************/
class DllExport DOSFAM : public TXTFAM {
 public:
  // Constructor
  DOSFAM(PDOSDEF tdp);
  DOSFAM(PDOSFAM txfp);
  DOSFAM(PBLKFAM tdfp, PDOSDEF tdp);

  // Implementation
  virtual AMT   GetAmType(void) {return TYPE_AM_DOS;}
  virtual bool  GetUseTemp(void) {return UseTemp;}
  virtual int   GetPos(void);
  virtual int   GetNextPos(void);
  virtual PTXF  Duplicate(PGLOBAL g)
                  {return (PTXF)new(g) DOSFAM(this);}

  // Methods
  virtual void  Reset(void);
  virtual int   GetFileLength(PGLOBAL g);
  virtual int   Cardinality(PGLOBAL g);
  virtual int   MaxBlkSize(PGLOBAL g, int s);
  virtual bool  AllocateBuffer(PGLOBAL g);
  virtual int   GetRowID(void);
  virtual bool  RecordPos(PGLOBAL g);
  virtual bool  SetPos(PGLOBAL g, int recpos);
  virtual int   SkipRecord(PGLOBAL g, bool header);
  virtual bool  OpenTableFile(PGLOBAL g);
  virtual int   ReadBuffer(PGLOBAL g);
  virtual int   WriteBuffer(PGLOBAL g);
  virtual int   DeleteRecords(PGLOBAL g, int irc);
  virtual void  CloseTableFile(PGLOBAL g, bool abort);
  virtual void  Rewind(void);

 protected:
  virtual bool  OpenTempFile(PGLOBAL g);
  virtual bool  MoveIntermediateLines(PGLOBAL g, bool *b);
  virtual int   RenameTempFile(PGLOBAL g);
  virtual int   InitDelete(PGLOBAL g, int fpos, int spos);

  // Members
  FILE   *Stream;             // Points to Dos file structure
  FILE   *T_Stream;           // Points to temporary file structure
  PFBLOCK To_Fbt;             // Pointer to temp file block
  bool    UseTemp;            // True to use a temporary file in Upd/Del
  bool    Bin;                // True to force binary mode
  }; // end of class DOSFAM

/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for standard  */
/*  text files with variable record format (DOS, CSV, FMT)             */
/***********************************************************************/
class DllExport BLKFAM : public DOSFAM {
 public:
  // Constructor
  BLKFAM(PDOSDEF tdp);
  BLKFAM(PBLKFAM txfp);

  // Implementation
  virtual AMT   GetAmType(void) {return TYPE_AM_BLK;}
  virtual int   GetPos(void);
  virtual int   GetNextPos(void);
  virtual PTXF  Duplicate(PGLOBAL g)
                  {return (PTXF)new(g) BLKFAM(this);}

  // Methods
  virtual void  Reset(void);
  virtual int   Cardinality(PGLOBAL g);
  virtual int   MaxBlkSize(PGLOBAL g, int s);
  virtual bool  AllocateBuffer(PGLOBAL g);
  virtual int   GetRowID(void);
  virtual bool  RecordPos(PGLOBAL g);
  virtual bool  SetPos(PGLOBAL g, int recpos);
  virtual int   SkipRecord(PGLOBAL g, bool header);
  virtual int   ReadBuffer(PGLOBAL g);
  virtual int   WriteBuffer(PGLOBAL g);
  virtual void  CloseTableFile(PGLOBAL g, bool abort);
  virtual void  Rewind(void);

 protected:
  // Members
  char *CurLine;              // Position of current line in buffer
  char *NxtLine;              // Position of Next    line in buffer
  char *OutBuf;               // Buffer to write in temporary file
  bool  Closing;              // True when closing on Update
  }; // end of class BLKFAM

/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for binary    */
/*  files with variable record format (BJSON)                          */
/***********************************************************************/
class DllExport BINFAM : public DOSFAM {
public:
	// Constructor
	BINFAM(PDOSDEF tdp) : DOSFAM(tdp) {Recsize = 0;}
	BINFAM(PBINFAM txfp) : DOSFAM(txfp) {Recsize = txfp->Recsize;}

	// Implementation
	virtual AMT   GetAmType(void) {return TYPE_AM_BIN;}
//virtual int   GetPos(void);
//virtual int   GetNextPos(void);
	virtual PTXF  Duplicate(PGLOBAL g) { return (PTXF)new(g) BINFAM(this); }

	// Methods
//virtual void  Reset(void) {TXTFAM::Reset();}
//virtual int   GetFileLength(PGLOBAL g);
//virtual int   Cardinality(PGLOBAL g);
	virtual int   MaxBlkSize(PGLOBAL g, int s) {return s;}
	virtual bool  AllocateBuffer(PGLOBAL g);
//virtual int   GetRowID(void);
//virtual bool  RecordPos(PGLOBAL g);
//virtual bool  SetPos(PGLOBAL g, int recpos);
	virtual int   SkipRecord(PGLOBAL g, bool header) {return RC_OK;}
//virtual bool  OpenTableFile(PGLOBAL g);
	virtual int   ReadBuffer(PGLOBAL g);
	virtual int   WriteBuffer(PGLOBAL g);
//virtual int   DeleteRecords(PGLOBAL g, int irc);
//virtual void  CloseTableFile(PGLOBAL g, bool abort);
//virtual void  Rewind(void);

//protected:
//virtual int   InitDelete(PGLOBAL g, int fpos, int spos);

	// Members
	size_t Recsize;		// Length of last read or next written record
}; // end of class BINFAM

#endif // __FILAMTXT_H
