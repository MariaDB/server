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
  AMT  GetAmType(void) override {return TYPE_AM_FIX;}
  PTXF Duplicate(PGLOBAL g) override
                 {return (PTXF)new(g) FIXFAM(this);}

  // Methods
  int  Cardinality(PGLOBAL g) override {return TXTFAM::Cardinality(g);}
  int  MaxBlkSize(PGLOBAL g, int s) override
                {return TXTFAM::MaxBlkSize(g, s);}
  bool SetPos(PGLOBAL g, int recpos) override;
  int  GetNextPos(void) override {return Fpos + 1;}
  bool AllocateBuffer(PGLOBAL g) override;
  void ResetBuffer(PGLOBAL g) override;
  virtual int  WriteModifiedBlock(PGLOBAL g);
  int  ReadBuffer(PGLOBAL g) override;
  int  WriteBuffer(PGLOBAL g) override;
  int  DeleteRecords(PGLOBAL g, int irc) override;
  void CloseTableFile(PGLOBAL g, bool abort) override;

 protected:
  virtual bool CopyHeader(PGLOBAL g) {return false;}
  bool MoveIntermediateLines(PGLOBAL g, bool *b) override;
  int  InitDelete(PGLOBAL g, int fpos, int spos) override;

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
  PTXF Duplicate(PGLOBAL g) override
                 {return (PTXF)new(g) BGXFAM(this);}

  // Methods
  int  Cardinality(PGLOBAL g) override;
  bool OpenTableFile(PGLOBAL g) override;
  int  WriteModifiedBlock(PGLOBAL g) override;
  int  ReadBuffer(PGLOBAL g) override;
  int  WriteBuffer(PGLOBAL g) override;
  int  DeleteRecords(PGLOBAL g, int irc) override;
  void CloseTableFile(PGLOBAL g, bool abort) override;
  void Rewind(void) override;

 protected:
  bool OpenTempFile(PGLOBAL g) override;
  bool MoveIntermediateLines(PGLOBAL g, bool *b = NULL) override;
          int  BigRead(PGLOBAL g, HANDLE h, void *inbuf, int req);
          bool BigWrite(PGLOBAL g, HANDLE h, void *inbuf, int req);
          bool BigSeek(PGLOBAL g, HANDLE h, BIGINT pos
                                          , int org = FILE_BEGIN);

  // Members
  HANDLE  Hfile;               // Handle(descriptor) to big file
  HANDLE  Tfile;               // Handle(descriptor) to big temp file
  }; // end of class BGXFAM

#endif // __FILAMFIX_H
