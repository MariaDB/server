/************** FilAMFix H Declares Source Code File (.H) **************/
/*  Name: FILAMFIX.H    Version 1.3                                    */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005 - 2014  */
/*                                                                     */
/*  This file contains the FIX file access method classes declares.    */
/***********************************************************************/

#ifndef __FILAMFIX_H
#define __FILAMFIX_H

#include "filamtxt.h"

typedef class FIXFAM *PFIXFAM;
typedef class BGXFAM *PBGXFAM;

/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for standard  */
/*  files with fixed record format (FIX, BIN)                          */
/***********************************************************************/
class DllExport FIXFAM : public BLKFAM {
 public:
  // Constructor
  FIXFAM(PDOSDEF tdp);
  FIXFAM(PFIXFAM txfp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_FIX;}
  virtual PTXF Duplicate(PGLOBAL g)
                 {return (PTXF)new(g) FIXFAM(this);}

  // Methods
  virtual int  Cardinality(PGLOBAL g) {return TXTFAM::Cardinality(g);}
  virtual int  MaxBlkSize(PGLOBAL g, int s)
                {return TXTFAM::MaxBlkSize(g, s);}
  virtual bool SetPos(PGLOBAL g, int recpos);
  virtual int  GetNextPos(void) {return Fpos + 1;}
  virtual bool AllocateBuffer(PGLOBAL g);
  virtual void ResetBuffer(PGLOBAL g);
  virtual int  WriteModifiedBlock(PGLOBAL g);
  virtual int  ReadBuffer(PGLOBAL g);
  virtual int  WriteBuffer(PGLOBAL g);
  virtual int  DeleteRecords(PGLOBAL g, int irc);
  virtual void CloseTableFile(PGLOBAL g, bool abort);

 protected:
  virtual bool CopyHeader(PGLOBAL g) {return false;}
  virtual bool MoveIntermediateLines(PGLOBAL g, bool *b);
  virtual int  InitDelete(PGLOBAL g, int fpos, int spos);

  // No additional members
  }; // end of class FIXFAM


/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for files     */
/*  that are standard files with columns starting at fixed offset      */
/*  This class is for fixed formatted files of more than 2 gigabytes.  */
/***********************************************************************/
class BGXFAM : public FIXFAM {
 public:
  // Constructor
  BGXFAM(PDOSDEF tdp);
  BGXFAM(PBGXFAM txfp);

  // Implementation
  virtual PTXF Duplicate(PGLOBAL g)
                 {return (PTXF)new(g) BGXFAM(this);}

  // Methods
  virtual int  Cardinality(PGLOBAL g);
  virtual bool OpenTableFile(PGLOBAL g);
  virtual int  WriteModifiedBlock(PGLOBAL g);
  virtual int  ReadBuffer(PGLOBAL g);
  virtual int  WriteBuffer(PGLOBAL g);
  virtual int  DeleteRecords(PGLOBAL g, int irc);
  virtual void CloseTableFile(PGLOBAL g, bool abort);
  virtual void Rewind(void);

 protected:
  virtual bool OpenTempFile(PGLOBAL g);
  virtual bool MoveIntermediateLines(PGLOBAL g, bool *b = NULL);
          int  BigRead(PGLOBAL g, HANDLE h, void *inbuf, int req);
          bool BigWrite(PGLOBAL g, HANDLE h, void *inbuf, int req);
          bool BigSeek(PGLOBAL g, HANDLE h, BIGINT pos
                                          , int org = FILE_BEGIN);

  // Members
  HANDLE  Hfile;               // Handle(descriptor) to big file
  HANDLE  Tfile;               // Handle(descriptor) to big temp file
  }; // end of class BGXFAM

#endif // __FILAMFIX_H
