/*************** FilAMap H Declares Source Code File (.H) **************/
/*  Name: FILAMAP.H     Version 1.2                                    */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2012    */
/*                                                                     */
/*  This file contains the MAP file access method classes declares.    */
/***********************************************************************/
#ifndef __FILAMAP_H
#define __FILAMAP_H

#include "block.h"
#include "filamtxt.h"

typedef class MAPFAM *PMAPFAM;

/***********************************************************************/
/*  This is the variable file access method using file mapping.        */
/***********************************************************************/
class DllExport MAPFAM : public TXTFAM {
 public:
  // Constructor
  MAPFAM(PDOSDEF tdp);
  MAPFAM(PMAPFAM tmfp);

  // Implementation
  virtual AMT   GetAmType(void) {return TYPE_AM_MAP;}
  virtual int   GetPos(void);
  virtual int   GetNextPos(void);
  virtual PTXF  Duplicate(PGLOBAL g)
                  {return (PTXF)new(g) MAPFAM(this);}

  // Methods
  virtual void  Reset(void);
  virtual int   GetFileLength(PGLOBAL g);
  virtual int   Cardinality(PGLOBAL g) {return (g) ? -1 : 0;}
  virtual int   GetRowID(void);
  virtual bool  RecordPos(PGLOBAL g);
  virtual bool  SetPos(PGLOBAL g, int recpos); 
  virtual int   SkipRecord(PGLOBAL g, bool header);
  virtual bool  OpenTableFile(PGLOBAL g);
  virtual bool  DeferReading(void) {return false;}
  virtual int   ReadBuffer(PGLOBAL g);
  virtual int   WriteBuffer(PGLOBAL g);
  virtual int    DeleteRecords(PGLOBAL g, int irc);
  virtual void  CloseTableFile(PGLOBAL g);
  virtual void  Rewind(void);

 protected:
  // Members
  char *Memory;               // Pointer on file mapping view.
  char *Mempos;               // Position of next data to read
  char *Fpos;                  // Position of last read record
  char *Tpos;                 // Target Position for delete move
  char *Spos;                 // Start position for delete move
  char *Top;                  // Mark end of file mapping view
  }; // end of class MAPFAM

/***********************************************************************/
/*  This is the blocked file access method using file mapping.         */
/***********************************************************************/
class DllExport MBKFAM : public MAPFAM {
 public:
  // Constructor
  MBKFAM(PDOSDEF tdp);
  MBKFAM(PMAPFAM tmfp) : MAPFAM(tmfp) {}

  // Implementation
  virtual PTXF Duplicate(PGLOBAL g)
                  {return (PTXF)new(g) MBKFAM(this);}

  // Methods
  virtual void Reset(void);
  virtual int  Cardinality(PGLOBAL g);
  virtual int  GetRowID(void);
  virtual int  SkipRecord(PGLOBAL g, bool header);
  virtual int  ReadBuffer(PGLOBAL g);
  virtual void Rewind(void);

 protected:
  // No additional members
  }; // end of class MBKFAM

/***********************************************************************/
/*  This is the fixed file access method using file mapping.           */
/***********************************************************************/
class DllExport MPXFAM : public MBKFAM {
 public:
  // Constructor
  MPXFAM(PDOSDEF tdp);
  MPXFAM(PMAPFAM tmfp) : MBKFAM(tmfp) {}

  // Implementation
  virtual int   GetPos(void);
  virtual PTXF  Duplicate(PGLOBAL g)
                  {return (PTXF)new(g) MPXFAM(this);}

  // Methods
  virtual int   Cardinality(PGLOBAL g) {return TXTFAM::Cardinality(g);}
  virtual bool  SetPos(PGLOBAL g, int recpos); 
  virtual bool  DeferReading(void) {return false;}
  virtual int   ReadBuffer(PGLOBAL g);
  virtual int   WriteBuffer(PGLOBAL g);

 protected:
  // No additional members
  }; // end of class MPXFAM

#endif // __FILAMAP_H
