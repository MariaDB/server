/*************** TabFmt H Declares Source Code File (.H) ***************/
/*  Name: TABFMT.H    Version 2.5                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2001-2016    */
/*                                                                     */
/*  This file contains the CSV and FMT classes declares.               */
/***********************************************************************/
#include "xtable.h"                     // Base class declares
#include "tabdos.h"

typedef class  TDBFMT    *PTDBFMT;

/***********************************************************************/
/*  Functions used externally.                                         */
/***********************************************************************/
DllExport PQRYRES CSVColumns(PGLOBAL g, PCSZ dp, PTOS topt, bool info);

/***********************************************************************/
/*  CSV table.                                                         */
/***********************************************************************/
class DllExport CSVDEF : public DOSDEF { /* Logical table description  */
  friend class TDBCSV;
  friend class TDBCCL;
	friend PQRYRES CSVColumns(PGLOBAL, PCSZ, PTOS, bool);
public:
  // Constructor
  CSVDEF(void);

  // Implementation
  const char *GetType(void) override {return "CSV";}
  char    GetSep(void) {return Sep;}
  char    GetQot(void) {return Qot;}

  // Methods
  bool DefineAM(PGLOBAL g, LPCSTR am, int poff) override;
  PTDB GetTable(PGLOBAL g, MODE mode) override;

 protected:
  // Members
  bool    Fmtd;               /* true for formatted files              */
//bool    Accept;             /* true if wrong lines are accepted      */
  bool    Header;             /* true if first line contains headers   */
//int     Maxerr;             /* Maximum number of bad records         */
  int     Quoted;             /* Quoting level for quoted fields       */
  char    Sep;                /* Separator for standard CSV files      */
  char    Qot;                /* Character for quoted strings          */
  }; // end of CSVDEF

/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for files     */
/*  that are CSV files with columns separated by the Sep character.    */
/***********************************************************************/
class DllExport TDBCSV : public TDBDOS {
  friend class CSVCOL;
	friend class MAPFAM;
	friend PQRYRES CSVColumns(PGLOBAL, PCSZ, PTOS, bool);
public:
  // Constructor
  TDBCSV(PCSVDEF tdp, PTXF txfp);
  TDBCSV(PGLOBAL g, PTDBCSV tdbp);

  // Implementation
  AMT  GetAmType(void) override {return TYPE_AM_CSV;}
  PTDB Duplicate(PGLOBAL g) override
                {return (PTDB)new(g) TDBCSV(g, this);}

  // Methods
  PTDB Clone(PTABS t) override;
//virtual bool IsUsingTemp(PGLOBAL g);
  int  GetBadLines(void) override {return (int)Nerr;}

  // Database routines
  PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
  bool OpenDB(PGLOBAL g) override;
  int  WriteDB(PGLOBAL g) override;
  int  CheckWrite(PGLOBAL g) override;
  int  ReadBuffer(PGLOBAL g) override;        // Physical file read

  // Specific routines
  int  EstimatedLength(void) override;
  bool SkipHeader(PGLOBAL g) override;
  virtual bool CheckErr(void);

 protected:
  bool PrepareWriting(PGLOBAL g) override;

  // Members
  PSZ  *Field;             // Field to write to current line
  int  *Offset;            // Column offsets for current record
  int  *Fldlen;            // Column field length for current record
  bool *Fldtyp;            // true for numeric fields
  int   Fields;            // Number of fields to handle
  int   Nerr;              // Number of bad records
  int   Maxerr;            // Maximum number of bad records
  int   Quoted;            // Quoting level for quoted fields
  bool  Accept;            // true if bad lines are accepted
  bool  Header;            // true if first line contains column headers
  char  Sep;               // Separator
  char  Qot;               // Quoting character
  }; // end of class TDBCSV

/***********************************************************************/
/*  Class CSVCOL: CSV access method column descriptor.                 */
/*  This A.M. is used for Comma Separated V(?) files.                  */
/***********************************************************************/
class DllExport CSVCOL : public DOSCOL {
  friend class TDBCSV;
  friend class TDBFMT;
 public:
  // Constructors
  CSVCOL(PGLOBAL g, PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i);
  CSVCOL(CSVCOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation
  int    GetAmType() override {return TYPE_AM_CSV;}

  // Methods
  bool   VarSize(void) override;
  void   ReadColumn(PGLOBAL g) override;
  void   WriteColumn(PGLOBAL g) override;

 protected:
  // Default constructor not to be used
  CSVCOL(void) = default;

  // Members
  int Fldnum;               // Field ordinal number (0 based)
  }; // end of class CSVCOL

/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for files     */
/*  whose record format is described by a Format keyword.              */
/***********************************************************************/
class DllExport TDBFMT : public TDBCSV {
  friend class CSVCOL;
//friend class FMTCOL;
 public:
  // Standard constructor
  TDBFMT(PCSVDEF tdp, PTXF txfp) : TDBCSV(tdp, txfp)
      {FldFormat = NULL; To_Fld = NULL; FmtTest = NULL; Linenum = 0;}

  // Copy constructor
  TDBFMT(PGLOBAL g, PTDBFMT tdbp);

  // Implementation
  AMT  GetAmType(void) override {return TYPE_AM_FMT;}
  PTDB Duplicate(PGLOBAL g) override
                {return (PTDB)new(g) TDBFMT(g, this);}

  // Methods
  PTDB Clone(PTABS t) override;

  // Database routines
  PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
//int  GetMaxSize(PGLOBAL g) override;
  bool OpenDB(PGLOBAL g) override;
  int  WriteDB(PGLOBAL g) override;
//virtual int  CheckWrite(PGLOBAL g);
  int  ReadBuffer(PGLOBAL g) override;        // Physical file read

  // Specific routines
  int  EstimatedLength(void) override;

 protected:
  bool PrepareWriting(PGLOBAL g) override 
       {snprintf(g->Message, sizeof(g->Message), MSG(TABLE_READ_ONLY), "FMT"); return true;}

  // Members
  PSZ  *FldFormat;                      // Field read format
  void *To_Fld;                         // To field test buffer
  int  *FmtTest;                        // Test on ending by %n or %m
  int   Linenum;                        // Last read line
  }; // end of class TDBFMT

/***********************************************************************/
/*  This is the class declaration for the CSV catalog table.           */
/***********************************************************************/
class DllExport TDBCCL : public TDBCAT {
 public:
  // Constructor
  TDBCCL(PCSVDEF tdp);

 protected:
  // Specific routines
  PQRYRES GetResult(PGLOBAL g) override;

  // Members
	PTOS  Topt;
}; // end of class TDBCCL

/* ------------------------- End of TabFmt.H ------------------------- */
