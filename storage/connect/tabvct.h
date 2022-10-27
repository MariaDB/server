/*************** TabVct H Declares Source Code File (.H) ***************/
/*  Name: TABVCT.H    Version 3.4                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1999-2011    */
/*                                                                     */
/*  This file contains the TDBVCT class  declares.                     */
/***********************************************************************/
#ifndef __TABVCT__
#define __TABVCT__

#include "tabfix.h"
#if defined(UNIX)
//#include <string.h.SUNWCCh>
#endif

typedef class TDBVCT *PTDBVCT;
typedef class VCTCOL *PVCTCOL;

/***********************************************************************/
/*  VCT table.                                                         */
/***********************************************************************/
class DllExport VCTDEF : public DOSDEF {  /* Logical table description */
  friend class TDBVCT;
  friend class VCTFAM;
  friend class VECFAM;
  friend class VMPFAM;
 public:
  // Constructor
  VCTDEF(void) {Split = false; Estimate = Header = 0;}

  // Implementation
  virtual const char *GetType(void) {return "VCT";}
  int  GetEstimate(void) {return Estimate;}

  // Methods
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE mode);

 protected:
          int  MakeFnPattern(char *fpat);

  // Members
  bool    Split;              /* Columns in separate files             */
  int     Estimate;           /* Estimated maximum size of table       */
  int     Header;             /* 0: no, 1: separate, 2: in data file   */
  }; // end of VCTDEF

/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for files     */
/*  in blocked vector format. In each block containing "Elements"      */
/*  records, values of each columns are consecutively stored (vector). */
/***********************************************************************/
class DllExport TDBVCT : public TDBFIX {
  friend class VCTCOL;
  friend class VCTFAM;
  friend class VCMFAM;
  friend class VECFAM;
  friend class VMPFAM;
 public:
  // Constructors
  TDBVCT(PVCTDEF tdp, PTXF txfp);
  TDBVCT(PGLOBAL g, PTDBVCT tdbp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_VCT;}
  virtual PTDB Duplicate(PGLOBAL g)
                {return (PTDB)new(g) TDBVCT(g, this);}
          bool IsSplit(void) {return ((VCTDEF*)To_Def)->Split;}

  // Methods
  virtual PTDB Clone(PTABS t);
  virtual bool IsUsingTemp(PGLOBAL g);

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual void CloseDB(PGLOBAL g);

 protected:
  // Members
  }; // end of class TDBVCT

/***********************************************************************/
/*  Class VCTCOL: VCT access method column descriptor.                 */
/*  This A.M. is used for file having column wise organization.        */
/***********************************************************************/
class DllExport VCTCOL : public DOSCOL {
  friend class TDBVCT;
  friend class VCTFAM;
  friend class VCMFAM;
  friend class VECFAM;
  friend class VMPFAM;
  friend class BGVFAM;
 public:
  // Constructors
  VCTCOL(PGLOBAL g, PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i);
  VCTCOL(VCTCOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation
  virtual int  GetAmType(void) {return TYPE_AM_VCT;}

  // Methods
  virtual void ReadColumn(PGLOBAL g);
  virtual void WriteColumn(PGLOBAL g);
  virtual bool SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check);
  virtual void SetOk(void);

 protected:
  virtual void ReadBlock(PGLOBAL g);
  virtual void WriteBlock(PGLOBAL g);

  VCTCOL(void) {}        // Default constructor not to be used

  // Members
  PVBLK Blk;             // Block buffer
  int   Clen;            // Internal length in table
  int   ColBlk;          // Block pointed by column
  int   ColPos;          // Last position read
  int   Modif;           // Number of modified lines in block
  }; // end of class VCTCOL

#endif // __TABVCT__

