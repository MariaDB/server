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
  AMT  GetAmType(void) override {return TYPE_AM_FIX;}
  void RestoreNrec(void) override;
  PTDB Duplicate(PGLOBAL g) override
                {return (PTDB)new(g) TDBFIX(g, this);}

  // Methods
  PTDB Clone(PTABS t) override;
  void ResetDB(void) override;
  bool IsUsingTemp(PGLOBAL g) override;
  int  RowNumber(PGLOBAL g, bool b = false) override;
  int  ResetTableOpt(PGLOBAL g, bool dop, bool dox) override;
  void ResetSize(void) override;
  int  GetBadLines(void) override {return Txfp->GetNerr();}

  // Database routines
  PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
  int  GetProgMax(PGLOBAL g) override;
  int  Cardinality(PGLOBAL g) override;
  int  GetMaxSize(PGLOBAL g) override;
  bool OpenDB(PGLOBAL g) override;
  int  WriteDB(PGLOBAL g) override;

 protected:
  bool PrepareWriting(PGLOBAL g) override {return false;}

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
  int  GetAmType(void) override {return TYPE_AM_BIN;}
          int  GetDeplac(void) {return Deplac;}
          int  GetFileSize(void) 
               {return N ? N : GetTypeSize(Buf_Type, Long);}

  // Methods
  void ReadColumn(PGLOBAL g) override;
  void WriteColumn(PGLOBAL g) override;

  // Static
  static  void SetEndian(void); 

 protected:
  BINCOL(void) = default;    // Default constructor not to be used

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
	PQRYRES GetResult(PGLOBAL g) override
	  {return DBFColumns(g, ((PTABDEF)To_Def)->GetPath(), Fn, Topt, false);}

	// Members
	PCSZ Fn;                    // The DBF file (path) name
	PTOS Topt;
}; // end of class TDBOCL


#endif // __TABFIX__
