/*************** Tabjdbc H Declares Source Code File (.H) **************/
/*  Name: TABJDBC.H   Version 1.1                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2016-2017    */
/*                                                                     */
/*  This file contains the TDBJDBC classes declares.                   */
/***********************************************************************/
#include "colblk.h"
#include "resource.h"
#include "jdbccat.h"

typedef class JDBCDEF *PJDBCDEF;
typedef class TDBJDBC *PTDBJDBC;
typedef class JDBCCOL *PJDBCCOL;
typedef class TDBXJDC *PTDBXJDC;
typedef class JSRCCOL *PJSRCCOL;

/***********************************************************************/
/*  JDBC table.                                                        */
/***********************************************************************/
class DllExport JDBCDEF : public EXTDEF { /* Logical table description */
	friend class TDBJDBC;
	friend class TDBXJDC;
	friend class TDBJDRV;
	friend class TDBJTB;
	friend class TDBJDBCL;
public:
	// Constructor
	JDBCDEF(void);

	// Implementation
	const char *GetType(void) override { return "JDBC"; }

	// Methods
	bool DefineAM(PGLOBAL g, LPCSTR am, int poff) override;
	PTDB GetTable(PGLOBAL g, MODE m) override;
	int  ParseURL(PGLOBAL g, char *url, bool b = true);
	bool SetParms(PJPARM sjp);

protected:
	// Members
	PSZ     Driver;             /* JDBC driver                           */
	PSZ     Url;                /* JDBC driver URL                       */
	PSZ     Wrapname;           /* Java driver name                      */
}; // end of JDBCDEF

#if !defined(NJDBC)
#include "jdbconn.h"

/***********************************************************************/
/*  This is the JDBC Access Method class declaration for files from    */
/*  other DB drivers to be accessed via JDBC.                          */
/***********************************************************************/
class TDBJDBC : public TDBEXT {
	friend class JDBCCOL;
	friend class JDBConn;
public:
	// Constructor
	TDBJDBC(PJDBCDEF tdp = NULL);
  TDBJDBC(PTDBJDBC tdbp);

	// Implementation
	AMT  GetAmType(void) override {return TYPE_AM_JDBC;}
  PTDB Duplicate(PGLOBAL g) override {return (PTDB)new(g) TDBJDBC(this);}

	// Methods
  PTDB Clone(PTABS t) override;
	bool SetRecpos(PGLOBAL g, int recpos) override;
	void ResetSize(void) override;
	PCSZ GetServer(void) override { return "JDBC"; }
	virtual int  Indexable(void) { return 2; }

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
	bool  SetParameters(PGLOBAL g);

	// Members
	JDBConn *Jcp;               // Points to a JDBC connection class
	JDBCCOL *Cnp;               // Points to count(*) column
	JDBCPARM Ops;               // Additional parameters
	PSZ      Wrapname;          // Points to Java wrapper name
	bool     Prepared;          // True when using prepared statement
	bool     Werr;							// Write error
	bool     Rerr;							// Rewind error
}; // end of class TDBJDBC

/***********************************************************************/
/*  Class JDBCCOL: JDBC access method column descriptor.               */
/*  This A.M. is used for JDBC tables.                                 */
/***********************************************************************/
class JDBCCOL : public EXTCOL {
	friend class TDBJDBC;
	friend class JDBConn;
public:
	// Constructors
	JDBCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am = "JDBC");
  JDBCCOL(JDBCCOL *colp, PTDB tdbp); // Constructor used in copy process

	// Implementation
	int  GetAmType(void) override { return TYPE_AM_JDBC; }

	// Methods
//virtual bool SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check);
	void ReadColumn(PGLOBAL g) override;
	void WriteColumn(PGLOBAL g) override;

protected:
	// Constructor for count(*) column
  JDBCCOL(void);

	// Members
	bool uuid;											 // For PostgreSQL
}; // end of class JDBCCOL

/***********************************************************************/
/*  This is the JDBC Access Method class declaration that send         */
/*  commands to be executed by other DB JDBC drivers.                  */
/***********************************************************************/
class TDBXJDC : public TDBJDBC {
	friend class JSRCCOL;
	friend class JDBConn;
public:
	// Constructors
	TDBXJDC(PJDBCDEF tdp = NULL);

	// Implementation
	AMT  GetAmType(void) override {return TYPE_AM_XDBC;}

	// Methods

	// Database routines
	PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
	//virtual int  GetProgMax(PGLOBAL g);
	int  GetMaxSize(PGLOBAL g) override;
	bool OpenDB(PGLOBAL g) override;
	int  ReadDB(PGLOBAL g) override;
	int  WriteDB(PGLOBAL g) override;
	int  DeleteDB(PGLOBAL g, int irc) override;
	//void CloseDB(PGLOBAL g) override;

protected:
	// Internal functions
	PCMD  MakeCMD(PGLOBAL g);

	// Members
	PCMD     Cmdlist;           // The commands to execute
	char    *Cmdcol;            // The name of the Xsrc command column
	int      Mxr;               // Maximum errors before closing
	int      Nerr;              // Number of errors so far
}; // end of class TDBXJDC

/***********************************************************************/
/*  Used by table in source execute mode.                              */
/***********************************************************************/
class JSRCCOL : public JDBCCOL {
	friend class TDBXJDC;
public:
	// Constructors
	JSRCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am = "JDBC");

	// Implementation
	int  GetAmType(void) override {return TYPE_AM_JDBC;}

	// Methods
	void ReadColumn(PGLOBAL g) override;
	void WriteColumn(PGLOBAL g) override;

protected:
	// Members
	char    *Buffer;              // To get returned message
	int      Flag;                // Column content desc
}; // end of class JSRCCOL

/***********************************************************************/
/*  This is the class declaration for the Drivers catalog table.       */
/***********************************************************************/
class TDBJDRV : public TDBCAT {
public:
	// Constructor
	TDBJDRV(PJDBCDEF tdp) : TDBCAT(tdp) {Maxres = tdp->Maxres;}

protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g) override;

	// Members
	int      Maxres;            // Returned lines limit
}; // end of class TDBJDRV

/***********************************************************************/
/*  This is the class declaration for the tables catalog table.        */
/***********************************************************************/
class TDBJTB : public TDBJDRV {
public:
	// Constructor
	TDBJTB(PJDBCDEF tdp);

protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g) override;

	// Members
	PCSZ     Schema;            // Points to schema name or NULL
	PCSZ     Tab;               // Points to JDBC table name or pattern
	PCSZ     Tabtype;           // Points to JDBC table type
	JDBCPARM Ops;               // Additional parameters
}; // end of class TDBJTB

/***********************************************************************/
/*  This is the class declaration for the columns catalog table.       */
/***********************************************************************/
class TDBJDBCL : public TDBJTB {
public:
	// Constructor
	TDBJDBCL(PJDBCDEF tdp);

protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g) override;

	// Members
	PCSZ Colpat;            // Points to catalog column pattern
}; // end of class TDBJDBCL

#endif // !NJDBC
