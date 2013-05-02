/*************** TabTbl H Declares Source Code File (.H) ***************/
/*  Name: TABTBL.H   Version 1.2                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2008-2013    */
/*                                                                     */
/*  This file contains the TDBTBL classes declares.                    */
/***********************************************************************/
#include "block.h"
#include "colblk.h"
#include "tabutil.h"

typedef class TBLDEF *PTBLDEF;
typedef class TDBTBL *PTDBTBL;

/***********************************************************************/
/*  TBL table.                                                         */
/***********************************************************************/
class DllExport TBLDEF : public PRXDEF {  /* Logical table description */
  friend class TDBTBL;
  friend class TDBTBC;
 public:
  // Constructor
  TBLDEF(void);

  // Implementation
  virtual const char *GetType(void) {return "TBL";}

  // Methods
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE m);

 protected:
  // Members
  bool    Accept;                  /* TRUE if bad tables are accepted  */
  int     Maxerr;                  /* Maximum number of bad tables     */
  int     Ntables;                 /* Number of tables                 */
  }; // end of TBLDEF

/***********************************************************************/
/*  This is the TBL Access Method class declaration.                   */
/***********************************************************************/
class DllExport TDBTBL : public TDBPRX {
  friend class TBTBLK;
  friend class TDBPLG;
 public:
  // Constructor
  TDBTBL(PTBLDEF tdp = NULL);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_TBL;}

  // Methods
  virtual void ResetDB(void);
  virtual int GetRecpos(void) {return Rows;}
  virtual int GetBadLines(void) {return (int)Nbf;}

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual int  RowNumber(PGLOBAL g, bool b = FALSE);
  virtual PCOL InsertSpecialColumn(PGLOBAL g, PCOL scp);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);

 protected:
  // Internal functions
  bool  InitTableList(PGLOBAL g);
  bool  TestFil(PGLOBAL g, PFIL filp, PTABLE tabp);

  // Members
  PTABLE  Tablist;              // Points to the table list
  PTABLE  CurTable;             // Points to the current table
  bool    Accept;               // TRUE if bad tables are accepted
  int     Maxerr;               // Maximum number of bad tables
  int     Nbf;                  // Number of bad connections
  int     Rows;                 // Used for RowID
  int     Crp;                  // Used for CurPos
  }; // end of class TDBTBL

/***********************************************************************/
/*  Class TBTBLK: TDBPLG TABID special column descriptor.              */
/***********************************************************************/
class TBTBLK : public TIDBLK {
 public:
  // The constructor must restore Value because XOBJECT has a void
  // constructor called by default that set Value to NULL
  TBTBLK(PVAL valp) {Value = valp;}

  // Methods
  virtual void ReadColumn(PGLOBAL g);

  // Fake operator new used to change TIDBLK into SDTBLK
  void * operator new(size_t size, TIDBLK *sp) {return sp;}

#if !defined(__BORLANDC__)
  // Avoid warning C4291 by defining a matching dummy delete operator
  void operator delete(void *, TIDBLK*) {}
#endif

 protected:
  // Must not have additional members
  }; // end of class TBTBLK
