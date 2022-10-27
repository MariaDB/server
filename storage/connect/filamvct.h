/************** FilAMVct H Declares Source Code File (.H) **************/
/*  Name: FILAMVCT.H    Version 1.5                                    */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2012    */
/*                                                                     */
/*  This file contains the VCT file access method classes declares.    */
/***********************************************************************/
#ifndef __FILAMVCT__
#define __FILAMVCT__

#include "filamfix.h"

typedef class VCTFAM *PVCTFAM;
typedef class VCTCOL *PVCTCOL;
typedef class VCMFAM *PVCMFAM;
typedef class VECFAM *PVECFAM;
typedef class VMPFAM *PVMPFAM;
typedef class BGVFAM *PBGVFAM;

/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for files     */
/*  in vector format. If MaxBlk=0, each block containing "Elements"    */
/*  records, values of each columns are consecutively stored (vector). */
/*  Otherwise, data is arranged by column in the file and MaxBlk is    */
/*  used to set the maximum number of blocks. This leave some white    */
/*  space allowing to insert new values up to this maximum size.       */
/***********************************************************************/
class DllExport VCTFAM : public FIXFAM {
  friend class TDBVCT;
  friend class VCTCOL;
 public:
  // Constructor
  VCTFAM(PVCTDEF tdp);
  VCTFAM(PVCTFAM txfp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_VCT;}
  virtual PTXF Duplicate(PGLOBAL g)
                {return (PTXF)new(g) VCTFAM(this);}
  virtual int  GetFileLength(PGLOBAL g);

  // Methods
  virtual void Reset(void);
  virtual int  MaxBlkSize(PGLOBAL g, int s);
  virtual bool AllocateBuffer(PGLOBAL g);
  virtual bool InitInsert(PGLOBAL g);
  virtual void ResetBuffer(PGLOBAL g) {}
  virtual int  Cardinality(PGLOBAL g);
  virtual int  GetRowID(void);

  // Database routines
  virtual bool OpenTableFile(PGLOBAL g);
  virtual int  ReadBuffer(PGLOBAL g);
  virtual int  WriteBuffer(PGLOBAL g);
  virtual int  DeleteRecords(PGLOBAL g, int irc);
  virtual void CloseTableFile(PGLOBAL g, bool abort);
  virtual void Rewind(void);

  // Specific functions
  virtual bool ReadBlock(PGLOBAL g, PVCTCOL colp);
  virtual bool WriteBlock(PGLOBAL g, PVCTCOL colp);

 protected:
  virtual bool MakeEmptyFile(PGLOBAL g, PCSZ fn);
  virtual bool OpenTempFile(PGLOBAL g);
  virtual bool MoveLines(PGLOBAL g) {return false;}
  virtual bool MoveIntermediateLines(PGLOBAL g, bool *b = NULL);
  virtual bool CleanUnusedSpace(PGLOBAL g);
  virtual int  GetBlockInfo(PGLOBAL g);
  virtual bool SetBlockInfo(PGLOBAL g);
          bool ResetTableSize(PGLOBAL g, int block, int last);

  // Members
  char   *NewBlock;         // To block written on Insert
  char   *Colfn;            // Pattern for column file names (VEC)
  char   *Tempat;           // Pattern for temp file names (VEC)
  int    *Clens;            // Pointer to col size array
  int    *Deplac;           // Pointer to col start position array
  bool   *Isnum;            // Pointer to buffer type isnum result
  bool    AddBlock;         // True when adding new blocks on Insert
  bool    Split;            // true: split column file vector format
  int     Header;           // 0: no, 1: separate, 2: in data file
  int     MaxBlk;           // Max number of blocks (True vector format)
  int     Bsize;            // Because Nrec can be modified
  int     Ncol;             // The number of columns;
  }; // end of class VCTFAM

/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for files     */
/*  in vector format accessed using file mapping.                      */
/***********************************************************************/
class DllExport VCMFAM : public VCTFAM {
  friend class TDBVCT;
  friend class VCTCOL;
 public:
  // Constructor
  VCMFAM(PVCTDEF tdp);
  VCMFAM(PVCMFAM txfp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_VMP;}
  virtual PTXF Duplicate(PGLOBAL g)
                {return (PTXF)new(g) VCMFAM(this);}

  // Methods
  virtual bool AllocateBuffer(PGLOBAL g);
  virtual bool InitInsert(PGLOBAL g);

  // Database routines
  virtual bool OpenTableFile(PGLOBAL g);
  virtual int  WriteBuffer(PGLOBAL g);
  virtual int  DeleteRecords(PGLOBAL g, int irc);
  virtual void CloseTableFile(PGLOBAL g, bool abort);

 protected:
  // Specific functions
  virtual bool MoveIntermediateLines(PGLOBAL g, bool *b = NULL);
  virtual bool ReadBlock(PGLOBAL g, PVCTCOL colp);
  virtual bool WriteBlock(PGLOBAL g, PVCTCOL colp);

  // Members
  char*   Memory;               // Pointer on file mapping view.
  char*  *Memcol;               // Pointer on column start.
  }; // end of class VCMFAM

/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for files     */
/*  in full vertical format. Each column is contained in a separate    */
/*  file whose name is the table name followed by the column number.   */
/***********************************************************************/
class DllExport VECFAM : public VCTFAM {
  friend class TDBVCT;
  friend class VCTCOL;
 public:
  // Constructor
  VECFAM(PVCTDEF tdp);
  VECFAM(PVECFAM txfp);

  // Implementation
  virtual PTXF Duplicate(PGLOBAL g)
                {return (PTXF)new(g) VECFAM(this);}

  // Methods
  virtual bool AllocateBuffer(PGLOBAL g);
  virtual bool InitInsert(PGLOBAL g);
  virtual void ResetBuffer(PGLOBAL g);

  // Database routines
  virtual bool OpenTableFile(PGLOBAL g);
  virtual int  WriteBuffer(PGLOBAL g);
  virtual int  DeleteRecords(PGLOBAL g, int irc);
  virtual void CloseTableFile(PGLOBAL g, bool abort);

  // Specific functions
  virtual bool ReadBlock(PGLOBAL g, PVCTCOL colp);
  virtual bool WriteBlock(PGLOBAL g, PVCTCOL colp);

 protected:
  virtual bool OpenTempFile(PGLOBAL g);
  virtual bool MoveLines(PGLOBAL g);
  virtual bool MoveIntermediateLines(PGLOBAL g, bool *b = NULL);
  virtual int  RenameTempFile(PGLOBAL g);
          bool OpenColumnFile(PGLOBAL g, PCSZ opmode, int i);

  // Members
  FILE*   *Streams;             // Points to Dos file structure array
  FILE*   *T_Streams;           // Points to temp file structure array
  PFBLOCK *To_Fbs;              // Pointer to file block array
  PFBLOCK *T_Fbs;               // Pointer to temp file block array
  void*   *To_Bufs;             // Pointer to col val block array
  bool     InitUpdate;          // Used to initialize updating
  }; // end of class VECFAM

/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for files     */
/*  in full vertical format accessed using file mapping.               */
/***********************************************************************/
class DllExport VMPFAM : public VCMFAM {
  friend class TDBVCT;
  friend class VCTCOL;
 public:
  // Constructor
  VMPFAM(PVCTDEF tdp);
  VMPFAM(PVMPFAM txfp);

  // Implementation
  virtual PTXF Duplicate(PGLOBAL g)
                {return (PTXF)new(g) VMPFAM(this);}

  // Methods
  virtual bool AllocateBuffer(PGLOBAL g);

  // Database routines
  virtual bool OpenTableFile(PGLOBAL g);
  virtual int  DeleteRecords(PGLOBAL g, int irc);
  virtual void CloseTableFile(PGLOBAL g, bool abort);

 protected:
          bool MapColumnFile(PGLOBAL g, MODE mode, int i);

  // Members
  PFBLOCK *To_Fbs;              // Pointer to file block array
  }; // end of class VMPFAM

/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for files     */
/*  in (possibly blocked) vector format that can be larger than 2GB.   */
/***********************************************************************/
class BGVFAM : public VCTFAM {
  friend class VCTCOL;
 public:
  // Constructors
  BGVFAM(PVCTDEF tdp);
  BGVFAM(PBGVFAM txfp);

  // Implementation
  virtual PTXF Duplicate(PGLOBAL g)
                {return (PTXF)new(g) BGVFAM(this);}

  // Methods
  virtual bool AllocateBuffer(PGLOBAL g);

  // Database routines
  virtual bool OpenTableFile(PGLOBAL g);
  virtual int  WriteBuffer(PGLOBAL g);
  virtual int  DeleteRecords(PGLOBAL g, int irc);
  virtual void CloseTableFile(PGLOBAL g, bool abort);
  virtual void Rewind(void);

  // Specific functions
  virtual bool ReadBlock(PGLOBAL g, PVCTCOL colp);
  virtual bool WriteBlock(PGLOBAL g, PVCTCOL colp);

 protected:
          bool BigSeek(PGLOBAL g, HANDLE h, BIGINT pos, bool b = false);
          bool BigRead(PGLOBAL g, HANDLE h, void *inbuf, int req);
          bool BigWrite(PGLOBAL g, HANDLE h, void *inbuf, int req);
  virtual bool MakeEmptyFile(PGLOBAL g, PCSZ fn);
  virtual bool OpenTempFile(PGLOBAL g);
  virtual bool MoveIntermediateLines(PGLOBAL g, bool *b = NULL);
  virtual bool CleanUnusedSpace(PGLOBAL g);
  virtual bool SetBlockInfo(PGLOBAL g);
  virtual int  GetBlockInfo(PGLOBAL g);

  // Members
  HANDLE  Hfile;                // Handle to big file
  HANDLE  Tfile;                // Handle to temporary file
  BIGINT *BigDep;               // Pointer to col start position array
  }; // end of class BGVFAM

#endif // __FILAMVCT__

