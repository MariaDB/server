/*************** TabDos H Declares Source Code File (.H) ***************/
/*  Name: TABFIX.H    Version 2.4                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1999-2015    */
/*                                                                     */
/*  This file contains the TDBFIX and (FIX/BIN)COL classes declares.   */
/***********************************************************************/
#ifndef __TABFIX__
#define __TABFIX__
#include "tabdos.h"             /* Base class declares                 */
#include "filamdbf.h"

typedef class FIXCOL *PFIXCOL;
typedef class BINCOL *PBINCOL;
typedef class TXTFAM *PTXF;

/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for files     */
/*  that are standard files with columns starting at fixed offset.     */
/*  This class is for fixed formatted files.                           */
/***********************************************************************/
class DllExport TDBFIX : public TDBDOS {
  friend class FIXCOL;
  friend class BINCOL;
 public:
  // Constructor
  TDBFIX(PDOSDEF tdp, PTXF txfp);
  TDBFIX(PGLOBAL g, PTDBFIX tdbp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_FIX;}
  virtual void RestoreNrec(void);
  virtual PTDB Duplicate(PGLOBAL g)
                {return (PTDB)new(g) TDBFIX(g, this);}

  // Methods
  virtual PTDB Clone(PTABS t);
  virtual void ResetDB(void);
  virtual bool IsUsingTemp(PGLOBAL g);
  virtual int  RowNumber(PGLOBAL g, bool b = false);
  virtual int  ResetTableOpt(PGLOBAL g, bool dop, bool dox);
  virtual void ResetSize(void);
  virtual int  GetBadLines(void) {return Txfp->GetNerr();}

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int  GetProgMax(PGLOBAL g);
  virtual int  Cardinality(PGLOBAL g);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  WriteDB(PGLOBAL g);

 protected:
  virtual bool PrepareWriting(PGLOBAL g) {return false;}

  // Members
  char Teds;                  /* Binary table default endian setting   */
  }; // end of class TDBFIX

/***********************************************************************/
/*  Class BINCOL: BIN access method column descriptor.                 */
/*  This A.M. is used for file processed by blocks.                    */
/***********************************************************************/
class DllExport BINCOL : public DOSCOL {
  friend class TDBFIX;
 public:
  // Constructors
  BINCOL(PGLOBAL g, PCOLDEF cdp, PTDB tp, PCOL cp, int i, PCSZ am = "BIN");
  BINCOL(BINCOL *colp, PTDB tdbp);  // Constructor used in copy process

  // Implementation
  virtual int  GetAmType(void) {return TYPE_AM_BIN;}
          int  GetDeplac(void) {return Deplac;}
          int  GetFileSize(void) 
               {return N ? N : GetTypeSize(Buf_Type, Long);}

  // Methods
  virtual void ReadColumn(PGLOBAL g);
  virtual void WriteColumn(PGLOBAL g);

  // Static
  static  void SetEndian(void); 

 protected:
  BINCOL(void) {}    // Default constructor not to be used

  // Members
  static char Endian;         // The host endian setting (L or B)
  char *Buff;                 // Utility buffer
  char  Eds;                  // The file endian setting
  char  Fmt;                  // The converted value format
  int   N;                    // The number of bytes in the file
  int   M;                    // The buffer type size
  int   Lim;                  // Min(N,M)
  }; // end of class BINCOL

/***********************************************************************/
/*  This is the class declaration for the DBF columns catalog table.   */
/***********************************************************************/
class TDBDCL : public TDBCAT {
public:
	// Constructor
	TDBDCL(PDOSDEF tdp) : TDBCAT(tdp)
	  {Fn = tdp->GetFn(); Topt = tdp->GetTopt();}

protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g)
	  {return DBFColumns(g, ((PTABDEF)To_Def)->GetPath(), Fn, Topt, false);}

	// Members
	PCSZ Fn;                    // The DBF file (path) name
	PTOS Topt;
}; // end of class TDBOCL


#endif // __TABFIX__
