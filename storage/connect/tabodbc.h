/*************** Tabodbc H Declares Source Code File (.H) **************/
/*  Name: TABODBC.H   Version 1.5                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2000-2013    */
/*                                                                     */
/*  This file contains the TDBODBC classes declares.                   */
/***********************************************************************/
#include "colblk.h"
#include "resource.h"

typedef class ODBCDEF *PODEF;
typedef class TDBODBC *PTDBODBC;
typedef class ODBCCOL *PODBCCOL;
typedef class TDBOIF  *PTDBOIF;
typedef class OIFCOL  *POIFCOL;
typedef class TDBSRC  *PTDBSRC;

/***********************************************************************/
/*  ODBC table.                                                        */
/***********************************************************************/
class DllExport ODBCDEF : public TABDEF { /* Logical table description */
 public:
  // Constructor
  ODBCDEF(void);

  // Implementation
  virtual const char *GetType(void) {return "ODBC";}
  PSZ  GetConnect(void) {return Connect;}
  PSZ  GetTabname(void) {return Tabname;}
  PSZ  GetTabowner(void) {return Tabowner;}
  PSZ  GetTabqual(void) {return Tabqual;}
  PSZ  GetQchar(void) {return (Qchar && *Qchar) ? Qchar : NULL;} 
  int  GetCatver(void) {return Catver;}
  int  GetOptions(void) {return Options;}

  // Methods
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE m);

 protected:
  // Members
  PSZ     Connect;            /* ODBC connection string                */
  PSZ     Tabname;            /* External table name                   */
  PSZ     Tabowner;           /* External table owner                  */
  PSZ     Tabqual;            /* External table qualifier              */
  PSZ     Qchar;              /* Identifier quoting character          */
  int     Catver;             /* ODBC version for catalog functions    */
  int     Options;            /* Open connection options               */
  }; // end of ODBCDEF

#if !defined(NODBC)
#include "odbconn.h"

/***********************************************************************/
/*  This is the ODBC Access Method class declaration for files from    */
/*  other DB drivers to be accessed via ODBC.                          */
/***********************************************************************/
class TDBODBC : public TDBASE {
  friend class ODBCCOL;
  friend class ODBConn;
 public:
  // Constructor
  TDBODBC(PODEF tdp = NULL);
  TDBODBC(PTDBODBC tdbp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_ODBC;}
  virtual PTDB Duplicate(PGLOBAL g)
                {return (PTDB)new(g) TDBODBC(this);}

  // Methods
  virtual PTDB CopyOne(PTABS t);
  virtual int  GetRecpos(void);
  virtual PSZ   GetFile(PGLOBAL g);
  virtual void SetFile(PGLOBAL g, PSZ fn);
  virtual void ResetSize(void);
  virtual int  GetAffectedRows(void) {return AftRows;}

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int  GetProgMax(PGLOBAL g);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual int  WriteDB(PGLOBAL g);
  virtual int  DeleteDB(PGLOBAL g, int irc);
  virtual void CloseDB(PGLOBAL g);

 protected:
  // Internal functions
  int Decode(char *utf, char *buf, size_t n);
  char *MakeSQL(PGLOBAL g, bool cnt);
//bool  MakeUpdate(PGLOBAL g, PSELECT selist);
//bool  MakeInsert(PGLOBAL g);
//bool  MakeDelete(PGLOBAL g);
//bool  MakeFilter(PGLOBAL g, bool c);
//bool  BindParameters(PGLOBAL g);

  // Members
  ODBConn *Ocp;               // Points to an ODBC connection class
  ODBCCOL *Cnp;               // Points to count(*) column
  char    *Connect;           // Points to connection string
  char    *TableName;         // Points to ODBC table name
  char    *Owner;             // Points to ODBC table Owner
  char    *Qualifier;         // Points to ODBC table Qualifier
  char    *Query;             // Points to SQL statement
  char    *Count;             // Points to count(*) SQL statement
//char    *Where;             // Points to local where clause
  char    *Quote;             // The identifier quoting character
  char    *MulConn;           // Used for multiple ODBC tables
  char    *DBQ;               // The address part of Connect string
  int      Options;           // Connect options
  int      Fpos;              // Position of last read record
  int      AftRows;           // The number of affected rows
  int      Rows;              // Rowset size
  int      Catver;            // Catalog ODBC version
  int      CurNum;            // Current buffer line number
  int      Rbuf;              // Number of lines read in buffer
  int      BufSize;           // Size of connect string buffer
  int      Nparm;             // The number of statement parameters
  }; // end of class TDBODBC

/***********************************************************************/
/*  Class ODBCCOL: DOS access method column descriptor.                */
/*  This A.M. is used for ODBC tables.                                 */
/***********************************************************************/
class ODBCCOL : public COLBLK {
  friend class TDBODBC;
 public:
  // Constructors
  ODBCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PSZ am = "ODBC");
  ODBCCOL(ODBCCOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation
  virtual int     GetAmType(void) {return TYPE_AM_ODBC;}
          SQLLEN *GetStrLen(void) {return StrLen;}
          int     GetRank(void) {return Rank;}
//        PVBLK   GetBlkp(void) {return Blkp;}

  // Methods
//virtual bool   CheckLocal(PTDB tdbp);
  virtual bool   SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check);
  virtual void   ReadColumn(PGLOBAL g);
  virtual void   WriteColumn(PGLOBAL g);
          void   AllocateBuffers(PGLOBAL g, int rows);
          void  *GetBuffer(DWORD rows);
          SWORD  GetBuflen(void);
//        void   Print(PGLOBAL g, FILE *, uint);

 protected:
  // Constructor used by GetMaxSize
  ODBCCOL(void);

  // Members
  TIMESTAMP_STRUCT *Sqlbuf;     // To get SQL_TIMESTAMP's
  void   *Bufp;                 // To extended buffer
  PVBLK   Blkp;                // To Value Block
//char    F_Date[12];          // Internal Date format
  PVAL    To_Val;              // To value used for Insert
  SQLLEN *StrLen;              // As returned by ODBC
  SQLLEN  Slen;                 // Used with Fetch
  int     Rank;                 // Rank (position) number in the query
  }; // end of class ODBCCOL

/***********************************************************************/
/*  This is the class declaration for the Data Sources catalog table.  */
/***********************************************************************/
class TDBSRC : public TDBCAT {
 public:
  // Constructor
  TDBSRC(PODEF tdp) : TDBCAT(tdp) {}

 protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g);

  }; // end of class TDBSRC

/***********************************************************************/
/*  This is the class declaration for the Drivers catalog table.       */
/***********************************************************************/
class TDBDRV : public TDBCAT {
 public:
  // Constructor
  TDBDRV(PODEF tdp) : TDBCAT(tdp) {}

 protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g);

  }; // end of class TDBDRV

/***********************************************************************/
/*  This is the class declaration for the tables catalog table.        */
/***********************************************************************/
class TDBOTB : public TDBCAT {
 public:
  // Constructor
  TDBOTB(PODEF tdp);

 protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g);

  // Members
  char    *Dsn;               // Points to connection string
  char    *Tab;               // Points to ODBC table name or pattern
  }; // end of class TDBOTB

/***********************************************************************/
/*  This is the class declaration for the columns catalog table.       */
/***********************************************************************/
class TDBOCL : public TDBOTB {
 public:
  // Constructor
   TDBOCL(PODEF tdp) : TDBOTB(tdp) {}

 protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g);

  // Members
  }; // end of class TDBOCL

#endif  // !NODBC
