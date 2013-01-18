/*************** TabTbl H Declares Source Code File (.H) ***************/
/*  Name: TABTBL.H   Version 1.2                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2008-2012    */
/*                                                                     */
/*  This file contains the TDBTBL classes declares.                    */
/***********************************************************************/
//#include "osutil.h"
#include "block.h"
#include "colblk.h"

typedef class TBLDEF *PTBLDEF;
typedef class TDBTBL *PTDBTBL;
typedef class TBLCOL *PTBLCOL;

/***********************************************************************/
/*  Defines the structure used for multiple tables.                 */
/***********************************************************************/
typedef struct _tablist *PTBL;

typedef struct _tablist {
  PTBL  Next;
  char *Name;
  char *DB;
  } TBLIST;

/***********************************************************************/
/*  TBL table.                                                         */
/***********************************************************************/
class DllExport TBLDEF : public TABDEF {  /* Logical table description */
  friend class TDBTBL;
 public:
  // Constructor
  TBLDEF(void);

  // Implementation
  virtual const char *GetType(void) {return "TBL";}
  PTBL GetTables(void) {return To_Tables;}
//int  GetNtables(void) {return Ntables;}

  // Methods
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE m);

 protected:
  // Members
  PTBL    To_Tables;               /* To the list of tables            */
  bool    Accept;                  /* TRUE if bad tables are accepted  */
  int     Maxerr;                  /* Maximum number of bad tables     */
  int     Ntables;                 /* Number of tables                 */
  }; // end of TBLDEF

/***********************************************************************/
/*  This is the TBL Access Method class declaration.                   */
/***********************************************************************/
class DllExport TDBTBL : public TDBASE {
  friend class TBLCOL;
  friend class TBTBLK;
	friend class TDBPLG;
 public:
  // Constructor
  TDBTBL(PTBLDEF tdp = NULL);
//TDBTBL(PTDBTBL tdbp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_TBL;}
//virtual PTDB Duplicate(PGLOBAL g)
//              {return (PTDB)new(g) TDBTBL(this);}

  // Methods
  virtual void ResetDB(void);
//virtual PTABLE GetTablist(void) {return (PSZ)Tablist;}
//virtual PTDB CopyOne(PTABS t);
  virtual int GetRecpos(void) {return Rows;}
  virtual int GetBadLines(void) {return (int)Nbf;}

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual int  GetProgMax(PGLOBAL g);
  virtual int  GetProgCur(void);
  virtual int  RowNumber(PGLOBAL g, bool b = FALSE);
  virtual PCOL InsertSpecialColumn(PGLOBAL g, PCOL scp);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual int  WriteDB(PGLOBAL g);
  virtual int  DeleteDB(PGLOBAL g, int irc);
  virtual void CloseDB(PGLOBAL g);

 protected:
  // Internal functions
	PTDB  GetSubTable(PGLOBAL g, PTBL tblp, PTABLE tabp);
  bool  InitTableList(PGLOBAL g);
	bool  TestFil(PGLOBAL g, PFIL filp, PTBL tblp);

  // Members
  PTABLE  Tablist;              // Points to the table list
  PTABLE  CurTable;             // Points to the current table
  PTDBASE Tdbp;                 // Current table PTDB
  bool    Accept;               // TRUE if bad tables are accepted
  int     Maxerr;               // Maximum number of bad tables
  int     Nbf;                  // Number of bad connections
  int     Rows;                 // Used for RowID
  int     Crp;                  // Used for CurPos
  }; // end of class TDBTBL

/***********************************************************************/
/*  Class TBLCOL: TBL access method column descriptor.                 */
/*  This A.M. is used for TBL tables.                                  */
/***********************************************************************/
class DllExport TBLCOL : public COLBLK {
  friend class TDBTBL;
 public:
  // Constructors
  TBLCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PSZ am = "TBL");
  TBLCOL(TBLCOL *colp, PTDB tdbp); // Constructor used in copy process
//TBLCOL(SPCBLK *colp, PTDB tdbp); // Constructor used for pseudo columns

  // Implementation
  virtual int    GetAmType(void) {return TYPE_AM_TBL;}

  // Methods
  virtual bool   IsSpecial(void) {return Pseudo;}
  virtual void   ReadColumn(PGLOBAL g);
//virtual void   WriteColumn(PGLOBAL g);
//        void   Print(PGLOBAL g, FILE *, UINT);
          bool   Init(PGLOBAL g);

 protected:
  // Default constructor not to be used
  TBLCOL(void) {}

  // Members
  PCOL     Colp;               // Points to matching table column
  PVAL     To_Val;             // To the matching column value
  bool     Pseudo;             // TRUE for special columns
  int      Colnum;             // Used when retrieving columns by number
  }; // end of class TBLCOL

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
