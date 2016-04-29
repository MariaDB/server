/*************** TabTbl H Declares Source Code File (.H) ***************/
/*  Name: TABTBL.H   Version 1.3                                       */
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
typedef class TDBTBM *PTDBTBM;
typedef class MYSQLC *PMYC;

/***********************************************************************/
/*  Defines the structures used for distributed TBM tables.            */
/***********************************************************************/
typedef struct _TBMtable *PTBMT;

typedef struct _TBMtable {
  PTBMT     Next;                 // Points to next data table struct
  PTABLE    Tap;                  // Points to the sub table
  PGLOBAL   G;                    // Needed in thread routine
  bool      Complete;             // TRUE when all results are read
  bool      Ready;                // TRUE when results are there
  int       Rows;                 // Total number of rows read so far
  int       ProgCur;              // Current pos
  int       ProgMax;              // Max pos
  int       Rc;                   // Return code
  THD      *Thd;
  pthread_attr_t attr;            // ???
  pthread_t Tid;                  // CheckOpen thread ID
  } TBMT;

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
  bool    Thread;                  /* Use thread for remote tables     */
  int     Maxerr;                  /* Maximum number of bad tables     */
  int     Ntables;                 /* Number of tables                 */
  }; // end of TBLDEF

/***********************************************************************/
/*  This is the TBL Access Method class declaration.                   */
/***********************************************************************/
class DllExport TDBTBL : public TDBPRX {
  friend class TBTBLK;
 public:
  // Constructor
  TDBTBL(PTBLDEF tdp = NULL);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_TBL;}

  // Methods
  virtual void ResetDB(void);
  virtual int  GetRecpos(void) {return Rows;}
  virtual int  GetBadLines(void) {return (int)Nbc;}

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int  Cardinality(PGLOBAL g);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual int  RowNumber(PGLOBAL g, bool b = FALSE);
  virtual PCOL InsertSpecialColumn(PCOL scp);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);

 protected:
  // Internal functions
  bool  InitTableList(PGLOBAL g);
  bool  TestFil(PGLOBAL g, PCFIL filp, PTABLE tabp);

  // Members
  PTABLE  Tablist;              // Points to the table list
  PTABLE  CurTable;             // Points to the current table
  bool    Accept;               // TRUE if bad tables are accepted
  int     Maxerr;               // Maximum number of bad tables
  int     Nbc;                  // Number of bad connections
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
  void operator delete(void *, size_t size) {}
#endif

 protected:
  // Must not have additional members
  }; // end of class TBTBLK

/***********************************************************************/
/*  This is the TBM Access Method class declaration.                   */
/***********************************************************************/
class DllExport TDBTBM : public TDBTBL {
  friend class TBTBLK;
 public:
  // Constructor
  TDBTBM(PTBLDEF tdp = NULL);

  // Methods
  virtual void ResetDB(void);

  // Database routines
	virtual int  Cardinality(PGLOBAL g) { return 10; }
	virtual int  GetMaxSize(PGLOBAL g) { return 10; } // Temporary
  virtual int  RowNumber(PGLOBAL g, bool b = FALSE);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);

 protected:
  // Internal functions
  bool  OpenTables(PGLOBAL g);
  int   ReadNextRemote(PGLOBAL g);

  // Members
  PTBMT Tmp;                  // To data table TBMT structures
  PTBMT Cmp;                  // Current data table PLGF (to move to TDBTBL)
  PTBMT Bmp;                  // To bad (unconnected) PLGF structures
  bool  Done;                 // TRUE after first GetAllResults
  int   Nrc;                  // Number of remote connections
  int   Nlc;                  // Number of local connections
  }; // end of class TDBTBM

pthread_handler_t ThreadOpen(void *p);
