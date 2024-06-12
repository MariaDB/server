/*************** FilAMap H Declares Source Code File (.H) **************/
/*  Name: FILAMAP.H     Version 1.3                                    */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2014    */
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
  friend class TDBJSON;
 public:
  // Constructor
  MAPFAM(PDOSDEF tdp);
  MAPFAM(PMAPFAM tmfp);

  // Implementation
  AMT   GetAmType(void) override {return TYPE_AM_MAP;}
  int   GetPos(void) override;
  int   GetNextPos(void) override;
  PTXF  Duplicate(PGLOBAL g) override
                  {return (PTXF)new(g) MAPFAM(this);}

  // Methods
  void  Reset(void) override;
  int   GetFileLength(PGLOBAL g) override;
  int   Cardinality(PGLOBAL g) override {return (g) ? -1 : 0;}
  int   MaxBlkSize(PGLOBAL g, int s) override {return s;}
  int   GetRowID(void) override;
  bool  RecordPos(PGLOBAL g) override;
  bool  SetPos(PGLOBAL g, int recpos) override;
  int   SkipRecord(PGLOBAL g, bool header) override;
  bool  OpenTableFile(PGLOBAL g) override;
  bool  DeferReading(void) override {return false;}
	virtual int   GetNext(PGLOBAL g) {return RC_EF;}
	int   ReadBuffer(PGLOBAL g) override;
  int   WriteBuffer(PGLOBAL g) override;
  int   DeleteRecords(PGLOBAL g, int irc) override;
  void  CloseTableFile(PGLOBAL g, bool abort) override;
  void  Rewind(void) override;

 protected:
  int   InitDelete(PGLOBAL g, int fpos, int spos) override;

  // Members
  char *Memory;               // Pointer on file mapping view.
  char *Mempos;               // Position of next data to read
  char *Fpos;                 // Position of last read record
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
  PTXF Duplicate(PGLOBAL g) override
                  {return (PTXF)new(g) MBKFAM(this);}

  // Methods
  void Reset(void) override;
  int  Cardinality(PGLOBAL g) override;
  int  MaxBlkSize(PGLOBAL g, int s) override
                {return TXTFAM::MaxBlkSize(g, s);}
  int  GetRowID(void) override;
  int  SkipRecord(PGLOBAL g, bool header) override;
  int  ReadBuffer(PGLOBAL g) override;
  void Rewind(void) override;

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
  int   GetPos(void) override;
  PTXF  Duplicate(PGLOBAL g) override
                  {return (PTXF)new(g) MPXFAM(this);}

  // Methods
  int   Cardinality(PGLOBAL g) override {return TXTFAM::Cardinality(g);}
  int   MaxBlkSize(PGLOBAL g, int s) override
                {return TXTFAM::MaxBlkSize(g, s);}
  bool  SetPos(PGLOBAL g, int recpos) override;
  int   GetNextPos(void) override {return GetPos() + 1;}
  bool  DeferReading(void) override {return false;}
  int   ReadBuffer(PGLOBAL g) override;
  int   WriteBuffer(PGLOBAL g) override;

 protected:
  int   InitDelete(PGLOBAL g, int fpos, int spos) override;

  // No additional members
  }; // end of class MPXFAM

#endif // __FILAMAP_H
