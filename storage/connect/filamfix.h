/************** FilAMFix H Declares Source Code File (.H) **************/
/*  Name: FILAMFIX.H    Version 1.2                                    */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005 - 2012  */
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
  virtual AMT   GetAmType(void) {return TYPE_AM_FIX;}
  virtual PTXF  Duplicate(PGLOBAL g)
                  {return (PTXF)new(g) FIXFAM(this);}

  // Methods
  virtual int  Cardinality(PGLOBAL g) {return TXTFAM::Cardinality(g);}
  virtual bool AllocateBuffer(PGLOBAL g);
  virtual void ResetBuffer(PGLOBAL g);
  virtual int  ReadBuffer(PGLOBAL g);
  virtual int  WriteBuffer(PGLOBAL g);
  virtual int  DeleteRecords(PGLOBAL g, int irc);
  virtual void CloseTableFile(PGLOBAL g);

 protected:
  virtual bool CopyHeader(PGLOBAL g) {return false;}
  virtual bool MoveIntermediateLines(PGLOBAL g, bool *b);

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
  virtual PTXF  Duplicate(PGLOBAL g)
                  {return (PTXF)new(g) BGXFAM(this);}

  // Methods
  virtual int   Cardinality(PGLOBAL g);
  virtual bool  OpenTableFile(PGLOBAL g);
  virtual int   ReadBuffer(PGLOBAL g);
  virtual int   WriteBuffer(PGLOBAL g);
  virtual int   DeleteRecords(PGLOBAL g, int irc);
  virtual void  CloseTableFile(PGLOBAL g);
  virtual void  Rewind(void);

 protected:
          bool BigSeek(PGLOBAL g, HANDLE h, BIGINT pos
                                          , int org = FILE_BEGIN);
          int  BigRead(PGLOBAL g, HANDLE h, void *inbuf, int req);
          bool BigWrite(PGLOBAL g, HANDLE h, void *inbuf, int req);
  virtual bool OpenTempFile(PGLOBAL g);
  virtual bool MoveIntermediateLines(PGLOBAL g, bool *b = NULL);

  // Members
  HANDLE  Hfile;               // Handle(descriptor) to big file
  HANDLE  Tfile;               // Handle(descriptor) to big temp file
  }; // end of class BGXFAM

#endif // __FILAMFIX_H
