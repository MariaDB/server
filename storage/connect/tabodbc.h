/*************** Tabodbc H Declares Source Code File (.H) **************/
/*  Name: TABODBC.H   Version 1.8                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2000-2015    */
/*                                                                     */
/*  This file contains the TDBODBC classes declares.                   */
/***********************************************************************/
#include "colblk.h"
#include "resource.h"

typedef class ODBCDEF *PODEF;
typedef class TDBODBC *PTDBODBC;
typedef class ODBCCOL *PODBCCOL;
typedef class TDBXDBC *PTDBXDBC;
typedef class XSRCCOL *PXSRCCOL;
typedef class TDBOIF  *PTDBOIF;
typedef class OIFCOL  *POIFCOL;
typedef class TDBSRC  *PTDBSRC;

/***********************************************************************/
/*  ODBC table.                                                        */
/***********************************************************************/
class DllExport ODBCDEF : public TABDEF { /* Logical table description */
  friend class TDBODBC;
  friend class TDBXDBC;
  friend class TDBDRV;
  friend class TDBOTB;
 public:
  // Constructor
  ODBCDEF(void);

  // Implementation
  virtual const char *GetType(void) {return "ODBC";}
  PSZ  GetConnect(void) {return Connect;}
  PSZ  GetTabname(void) {return Tabname;}
  PSZ  GetTabschema(void) {return Tabschema;}
  PSZ  GetTabcat(void) {return Tabcat;}
  PSZ  GetSrcdef(void) {return Srcdef;}
  char GetSep(void) {return (Sep) ? *Sep : 0;}
  int  GetQuoted(void) {return Quoted;} 
  int  GetCatver(void) {return Catver;}
  int  GetOptions(void) {return Options;}

  // Methods
	virtual int  Indexable(void) {return 2;}
	virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE m);

 protected:
  // Members
  PSZ     Connect;            /* ODBC connection string                */
  PSZ     Tabname;            /* External table name                   */
  PSZ     Tabschema;          /* External table schema                 */
  PSZ     Username;           /* User connect name                     */
  PSZ     Password;           /* Password connect info                 */
  PSZ     Tabcat;             /* External table catalog                */
  PSZ     Srcdef;             /* The source table SQL definition       */
  PSZ     Qchar;              /* Identifier quoting character          */
  PSZ     Qrystr;             /* The original query                    */
  PSZ     Sep;                /* Decimal separator                     */
  int     Catver;             /* ODBC version for catalog functions    */
  int     Options;            /* Open connection options               */
  int     Cto;                /* Open connection timeout               */
  int     Qto;                /* Query (command) timeout               */
  int     Quoted;             /* Identifier quoting level              */
  int     Maxerr;             /* Maxerr for an Exec table              */
  int     Maxres;             /* Maxres for a catalog table            */
  int     Memory;             /* Put result set in memory              */
  bool    Scrollable;         /* Use scrollable cursor                 */
  bool    Xsrc;               /* Execution type                        */
  bool    UseCnc;             /* Use SQLConnect (!SQLDriverConnect)    */
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
  virtual bool SetRecpos(PGLOBAL g, int recpos);
  virtual PSZ  GetFile(PGLOBAL g);
  virtual void SetFile(PGLOBAL g, PSZ fn);
  virtual void ResetSize(void);
//virtual int  GetAffectedRows(void) {return AftRows;}
  virtual PSZ  GetServer(void) {return "ODBC";}
  virtual int  Indexable(void) {return 2;}

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int  Cardinality(PGLOBAL g);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual int  GetProgMax(PGLOBAL g);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual int  WriteDB(PGLOBAL g);
  virtual int  DeleteDB(PGLOBAL g, int irc);
  virtual void CloseDB(PGLOBAL g);
	virtual bool ReadKey(PGLOBAL g, OPVAL op, const key_range *kr);

 protected:
  // Internal functions
  int   Decode(char *utf, char *buf, size_t n);
  bool  MakeSQL(PGLOBAL g, bool cnt);
  bool  MakeInsert(PGLOBAL g);
  bool  MakeCommand(PGLOBAL g);
//bool  MakeFilter(PGLOBAL g, bool c);
  bool  BindParameters(PGLOBAL g);
//char *MakeUpdate(PGLOBAL g);
//char *MakeDelete(PGLOBAL g);

  // Members
  ODBConn *Ocp;               // Points to an ODBC connection class
  ODBCCOL *Cnp;               // Points to count(*) column
  ODBCPARM Ops;               // Additional parameters
	PSTRG    Query;             // Constructed SQL query
	char    *Connect;           // Points to connection string
  char    *TableName;         // Points to ODBC table name
  char    *Schema;            // Points to ODBC table Schema
  char    *User;              // User connect info
  char    *Pwd;               // Password connect info
  char    *Catalog;           // Points to ODBC table Catalog
  char    *Srcdef;            // The source table SQL definition
  char    *Count;             // Points to count(*) SQL statement
//char    *Where;             // Points to local where clause
  char    *Quote;             // The identifier quoting character
  char    *MulConn;           // Used for multiple ODBC tables
  char    *DBQ;               // The address part of Connect string
  char    *Qrystr;            // The original query
  char     Sep;               // The decimal separator
  int      Options;           // Connect options
  int      Cto;               // Connect timeout
  int      Qto;               // Query timeout
  int      Quoted;            // The identifier quoting level
  int      Fpos;              // Position of last read record
  int      Curpos;            // Cursor position of last fetch
  int      AftRows;           // The number of affected rows
  int      Rows;              // Rowset size
  int      Catver;            // Catalog ODBC version
  int      CurNum;            // Current buffer line number
  int      Rbuf;              // Number of lines read in buffer
  int      BufSize;           // Size of connect string buffer
  int      Nparm;             // The number of statement parameters
  int      Memory;            // 0: No 1: Alloc 2: Put 3: Get
  bool     Scrollable;        // Use scrollable cursor
  bool     Placed;            // True for position reading
  bool     UseCnc;            // Use SQLConnect (!SQLDriverConnect)
  PQRYRES  Qrp;               // Points to storage result
  }; // end of class TDBODBC

/***********************************************************************/
/*  Class ODBCCOL: ODBC access method column descriptor.               */
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
          void    SetCrp(PCOLRES crp) {Crp = crp;}

  // Methods
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
  TIMESTAMP_STRUCT *Sqlbuf;    // To get SQL_TIMESTAMP's
  PCOLRES Crp;                 // To storage result
  void   *Bufp;                // To extended buffer
  PVBLK   Blkp;                // To Value Block
//char    F_Date[12];          // Internal Date format
  PVAL    To_Val;              // To value used for Insert
  SQLLEN *StrLen;              // As returned by ODBC
  SQLLEN  Slen;                // Used with Fetch
  int     Rank;                // Rank (position) number in the query
  }; // end of class ODBCCOL

/***********************************************************************/
/*  This is the ODBC Access Method class declaration that send         */
/*  commands to be executed by other DB ODBC drivers.                  */
/***********************************************************************/
class TDBXDBC : public TDBODBC {
  friend class XSRCCOL;
  friend class ODBConn;
 public:
  // Constructors
  TDBXDBC(PODEF tdp = NULL);
  TDBXDBC(PTDBXDBC tdbp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_XDBC;}
  virtual PTDB Duplicate(PGLOBAL g)
                {return (PTDB)new(g) TDBXDBC(this);}

  // Methods
  virtual PTDB CopyOne(PTABS t);
//virtual int  GetRecpos(void);
//virtual PSZ  GetFile(PGLOBAL g);
//virtual void SetFile(PGLOBAL g, PSZ fn);
//virtual void ResetSize(void);
//virtual int  GetAffectedRows(void) {return AftRows;}
//virtual PSZ  GetServer(void) {return "ODBC";}

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
//virtual int  GetProgMax(PGLOBAL g);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual int  WriteDB(PGLOBAL g);
  virtual int  DeleteDB(PGLOBAL g, int irc);
//virtual void CloseDB(PGLOBAL g);

 protected:
  // Internal functions
  PCMD  MakeCMD(PGLOBAL g);
//bool  BindParameters(PGLOBAL g);

  // Members
  PCMD     Cmdlist;           // The commands to execute
  char    *Cmdcol;            // The name of the Xsrc command column
  int      Mxr;               // Maximum errors before closing
  int      Nerr;              // Number of errors so far
  }; // end of class TDBXDBC

/***********************************************************************/
/*  Used by table in source execute mode.                              */
/***********************************************************************/
class XSRCCOL : public ODBCCOL {
  friend class TDBXDBC;
 public:
  // Constructors
  XSRCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PSZ am = "ODBC");
  XSRCCOL(XSRCCOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation
//virtual int  GetAmType(void) {return TYPE_AM_ODBC;}

  // Methods
  virtual void ReadColumn(PGLOBAL g);
  virtual void WriteColumn(PGLOBAL g);
//        void Print(PGLOBAL g, FILE *, uint);

 protected:
  // Members
  char    *Buffer;              // To get returned message
  int      Flag;                // Column content desc
  }; // end of class XSRCCOL

/***********************************************************************/
/*  This is the class declaration for the Drivers catalog table.       */
/***********************************************************************/
class TDBDRV : public TDBCAT {
 public:
  // Constructor
  TDBDRV(PODEF tdp) : TDBCAT(tdp) {Maxres = tdp->Maxres;}

 protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g);

  // Members
  int      Maxres;            // Returned lines limit
  }; // end of class TDBDRV

/***********************************************************************/
/*  This is the class declaration for the Data Sources catalog table.  */
/***********************************************************************/
class TDBSRC : public TDBDRV {
 public:
  // Constructor
  TDBSRC(PODEF tdp) : TDBDRV(tdp) {}

 protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g);

  // No additional Members
  }; // end of class TDBSRC

/***********************************************************************/
/*  This is the class declaration for the tables catalog table.        */
/***********************************************************************/
class TDBOTB : public TDBDRV {
 public:
  // Constructor
  TDBOTB(PODEF tdp);

 protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g);

  // Members
  char    *Dsn;               // Points to connection string
  char    *Schema;            // Points to schema name or NULL
  char    *Tab;               // Points to ODBC table name or pattern
  ODBCPARM Ops;               // Additional parameters
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

  // No additional Members
  }; // end of class TDBOCL

#endif  // !NODBC
