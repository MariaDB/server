/*************** Tabodbc H Declares Source Code File (.H) **************/
/*  Name: TABODBC.H   Version 1.9                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2000-2017    */
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
class DllExport ODBCDEF : public EXTDEF { /* Logical table description */
  friend class TDBODBC;
  friend class TDBXDBC;
  friend class TDBDRV;
  friend class TDBOTB;
	friend class TDBOCL;
public:
  // Constructor
  ODBCDEF(void);

  // Implementation
  const char *GetType(void) override {return "ODBC";}
  PSZ  GetConnect(void) {return Connect;}
  int  GetCatver(void) {return Catver;}

  // Methods
  int  Indexable(void) override {return 2;}
  bool DefineAM(PGLOBAL g, LPCSTR am, int poff) override;
  PTDB GetTable(PGLOBAL g, MODE m) override;

 protected:
  // Members
  PSZ     Connect;            /* ODBC connection string                */
  int     Catver;             /* ODBC version for catalog functions    */
  bool    UseCnc;             /* Use SQLConnect (!SQLDriverConnect)    */
  }; // end of ODBCDEF

#if !defined(NODBC)
#include "odbconn.h"

/***********************************************************************/
/*  This is the ODBC Access Method class declaration for files from    */
/*  other DB drivers to be accessed via ODBC.                          */
/***********************************************************************/
class TDBODBC : public TDBEXT {
  friend class ODBCCOL;
  friend class ODBConn;
 public:
  // Constructor
  TDBODBC(PODEF tdp = NULL);
  TDBODBC(PTDBODBC tdbp);

  // Implementation
  AMT  GetAmType(void) override {return TYPE_AM_ODBC;}
  PTDB Duplicate(PGLOBAL g) override
                {return (PTDB)new(g) TDBODBC(this);}

  // Methods
  PTDB Clone(PTABS t) override;
  bool SetRecpos(PGLOBAL g, int recpos) override;
  PCSZ GetFile(PGLOBAL g) override;
  void SetFile(PGLOBAL g, PCSZ fn) override;
  void ResetSize(void) override;
  PCSZ GetServer(void) override {return "ODBC";}
  virtual int  Indexable(void) {return 2;}

  // Database routines
  PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
  int  Cardinality(PGLOBAL g) override;
  bool OpenDB(PGLOBAL g) override;
  int  ReadDB(PGLOBAL g) override;
  int  WriteDB(PGLOBAL g) override;
  int  DeleteDB(PGLOBAL g, int irc) override;
  void CloseDB(PGLOBAL g) override;
  bool ReadKey(PGLOBAL g, OPVAL op, const key_range *kr) override;

 protected:
  // Internal functions
  bool  MakeInsert(PGLOBAL g);
  bool  BindParameters(PGLOBAL g);

  // Members
  ODBConn *Ocp;               // Points to an ODBC connection class
  ODBCCOL *Cnp;               // Points to count(*) column
  ODBCPARM Ops;               // Additional parameters
	char    *Connect;           // Points to connection string
  int      Catver;            // Catalog ODBC version
  bool     UseCnc;            // Use SQLConnect (!SQLDriverConnect)
  }; // end of class TDBODBC

/***********************************************************************/
/*  Class ODBCCOL: ODBC access method column descriptor.               */
/*  This A.M. is used for ODBC tables.                                 */
/***********************************************************************/
class ODBCCOL : public EXTCOL {
  friend class TDBODBC;
 public:
  // Constructors
  ODBCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am = "ODBC");
  ODBCCOL(ODBCCOL *colp, PTDB tdbp); // Constructor used in opy process

  // Implementation
  int     GetAmType(void) override {return TYPE_AM_ODBC;}
  SQLLEN *GetStrLen(void) {return StrLen;}

  // Methods
//virtual bool   SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check);
  void   ReadColumn(PGLOBAL g) override;
  void   WriteColumn(PGLOBAL g) override;
          void   AllocateBuffers(PGLOBAL g, int rows);
          void  *GetBuffer(DWORD rows);
          SWORD  GetBuflen(void);

 protected:
  // Constructor for count(*) column
  ODBCCOL(void);

  // Members
  TIMESTAMP_STRUCT *Sqlbuf;    // To get SQL_TIMESTAMP's
  SQLLEN *StrLen;              // As returned by ODBC
  SQLLEN  Slen;                // Used with Fetch
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
  AMT  GetAmType(void) override {return TYPE_AM_XDBC;}
  PTDB Duplicate(PGLOBAL g) override
                {return (PTDB)new(g) TDBXDBC(this);}

  // Methods
  PTDB Clone(PTABS t) override;

  // Database routines
  PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
  int  GetMaxSize(PGLOBAL g) override;
  bool OpenDB(PGLOBAL g) override;
  int  ReadDB(PGLOBAL g) override;
  int  WriteDB(PGLOBAL g) override;
  int  DeleteDB(PGLOBAL g, int irc) override;

 protected:
  // Internal functions
  PCMD  MakeCMD(PGLOBAL g);

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
  XSRCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am = "ODBC");
  XSRCCOL(XSRCCOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation

  // Methods
  void ReadColumn(PGLOBAL g) override;
  void WriteColumn(PGLOBAL g) override;

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
  PQRYRES GetResult(PGLOBAL g) override;

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
  PQRYRES GetResult(PGLOBAL g) override;

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
  PQRYRES GetResult(PGLOBAL g) override;

  // Members
	PCSZ     Dsn;               // Points to connection string
	PCSZ     Schema;            // Points to schema name or NULL
	PCSZ     Tab;               // Points to ODBC table name or pattern
	PCSZ     Tabtyp;            // Points to ODBC table type
	ODBCPARM Ops;               // Additional parameters
  }; // end of class TDBOTB

/***********************************************************************/
/*  This is the class declaration for the columns catalog table.       */
/***********************************************************************/
class TDBOCL : public TDBOTB {
 public:
  // Constructor
	 TDBOCL(PODEF tdp);

 protected:
  // Specific routines
  PQRYRES GetResult(PGLOBAL g) override;

  // Members
	char    *Colpat;						// Points to column pattern
  }; // end of class TDBOCL

#endif  // !NODBC
