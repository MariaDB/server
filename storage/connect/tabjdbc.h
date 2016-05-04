/*************** Tabjdbc H Declares Source Code File (.H) **************/
/*  Name: TABJDBC.H   Version 1.0                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2016         */
/*                                                                     */
/*  This file contains the TDBJDBC classes declares.                   */
/***********************************************************************/
#include "colblk.h"
#include "resource.h"

typedef class JDBCDEF *PJDBCDEF;
typedef class TDBJDBC *PTDBJDBC;
typedef class JDBCCOL *PJDBCCOL;
typedef class TDBXJDC *PTDBXJDC;
typedef class JSRCCOL *PJSRCCOL;
//typedef class TDBOIF  *PTDBOIF;
//typedef class OIFCOL  *POIFCOL;
//typedef class TDBJSRC *PTDBJSRC;

/***********************************************************************/
/*  JDBC table.                                                        */
/***********************************************************************/
class DllExport JDBCDEF : public TABDEF { /* Logical table description */
	friend class TDBJDBC;
	friend class TDBXJDC;
	friend class TDBJDRV;
	friend class TDBJTB;
public:
	// Constructor
	JDBCDEF(void);

	// Implementation
	virtual const char *GetType(void) { return "JDBC"; }
	PSZ  GetJpath(void) { return Jpath; }
	PSZ  GetTabname(void) { return Tabname; }
	PSZ  GetTabschema(void) { return Tabschema; }
	PSZ  GetTabcat(void) { return Tabcat; }
	PSZ  GetSrcdef(void) { return Srcdef; }
	char GetSep(void) { return (Sep) ? *Sep : 0; }
	int  GetQuoted(void) { return Quoted; }
//int  GetCatver(void) { return Catver; }
	int  GetOptions(void) { return Options; }

	// Methods
	virtual int  Indexable(void) { return 2; }
	virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
	virtual PTDB GetTable(PGLOBAL g, MODE m);

protected:
	// Members
	PSZ     Jpath;              /* Java class path                       */
	PSZ     Driver;             /* JDBC driver                           */
	PSZ     Url;                /* JDBC driver URL                       */
	PSZ     Tabname;            /* External table name                   */
	PSZ     Tabschema;          /* External table schema                 */
	PSZ     Username;           /* User connect name                     */
	PSZ     Password;           /* Password connect info                 */
	PSZ     Tabcat;             /* External table catalog                */
	PSZ     Tabtype;            /* External table type                   */
	PSZ     Srcdef;             /* The source table SQL definition       */
	PSZ     Qchar;              /* Identifier quoting character          */
	PSZ     Qrystr;             /* The original query                    */
	PSZ     Sep;                /* Decimal separator                     */
	int     Options;            /* Open connection options               */
//int     Cto;                /* Open connection timeout               */
//int     Qto;                /* Query (command) timeout               */
	int     Quoted;             /* Identifier quoting level              */
	int     Maxerr;             /* Maxerr for an Exec table              */
	int     Maxres;             /* Maxres for a catalog table            */
	int     Memory;             /* Put result set in memory              */
	bool    Scrollable;         /* Use scrollable cursor                 */
	bool    Xsrc;               /* Execution type                        */
}; // end of JDBCDEF

#if !defined(NJDBC)
#include "JDBConn.h"

/***********************************************************************/
/*  This is the JDBC Access Method class declaration for files from    */
/*  other DB drivers to be accessed via JDBC.                          */
/***********************************************************************/
class TDBJDBC : public TDBASE {
	friend class JDBCCOL;
	friend class JDBConn;
public:
	// Constructor
	TDBJDBC(PJDBCDEF tdp = NULL);
	TDBJDBC(PTDBJDBC tdbp);

	// Implementation
	virtual AMT  GetAmType(void) { return TYPE_AM_JDBC; }
	virtual PTDB Duplicate(PGLOBAL g) { return (PTDB)new(g)TDBJDBC(this); }

	// Methods
	virtual PTDB CopyOne(PTABS t);
	virtual int  GetRecpos(void);
	virtual bool SetRecpos(PGLOBAL g, int recpos);
//virtual PSZ  GetFile(PGLOBAL g);
//virtual void SetFile(PGLOBAL g, PSZ fn);
	virtual void ResetSize(void);
	//virtual int  GetAffectedRows(void) {return AftRows;}
	virtual PSZ  GetServer(void) { return "JDBC"; }
	virtual int  Indexable(void) { return 2; }

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
	bool  SetParameters(PGLOBAL g);
	//char *MakeUpdate(PGLOBAL g);
	//char *MakeDelete(PGLOBAL g);

	// Members
	JDBConn *Jcp;               // Points to a JDBC connection class
	JDBCCOL *Cnp;               // Points to count(*) column
	JDBCPARM Ops;               // Additional parameters
	PSTRG    Query;             // Constructed SQL query
	char    *Jpath;             // Java class path
	char    *TableName;         // Points to JDBC table name
	char    *Schema;            // Points to JDBC table Schema
	char    *User;              // User connect info
	char    *Pwd;               // Password connect info
	char    *Catalog;           // Points to JDBC table Catalog
	char    *Srcdef;            // The source table SQL definition
	char    *Count;             // Points to count(*) SQL statement
//char    *Where;             // Points to local where clause
	char    *Quote;             // The identifier quoting character
	char    *MulConn;           // Used for multiple JDBC tables
	char    *DBQ;               // The address part of Connect string
	char    *Qrystr;            // The original query
	char     Sep;               // The decimal separator
	int      Options;           // Connect options
//int      Cto;               // Connect timeout
//int      Qto;               // Query timeout
	int      Quoted;            // The identifier quoting level
	int      Fpos;              // Position of last read record
	int      Curpos;            // Cursor position of last fetch
	int      AftRows;           // The number of affected rows
	int      Rows;              // Rowset size
	int      CurNum;            // Current buffer line number
	int      Rbuf;              // Number of lines read in buffer
	int      BufSize;           // Size of connect string buffer
	int      Ncol;							// The column number
	int      Nparm;             // The number of statement parameters
	int      Memory;            // 0: No 1: Alloc 2: Put 3: Get
//bool     Scrollable;        // Use scrollable cursor --> in Ops
	bool     Placed;            // True for position reading
	bool     Prepared;          // True when using prepared statement
	bool     Werr;							// Write error
	bool     Rerr;							// Rewind error
	PQRYRES  Qrp;               // Points to storage result
}; // end of class TDBJDBC

/***********************************************************************/
/*  Class JDBCCOL: JDBC access method column descriptor.               */
/*  This A.M. is used for JDBC tables.                                 */
/***********************************************************************/
class JDBCCOL : public COLBLK {
	friend class TDBJDBC;
public:
	// Constructors
	JDBCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PSZ am = "JDBC");
	JDBCCOL(JDBCCOL *colp, PTDB tdbp); // Constructor used in copy process

	// Implementation
	virtual int     GetAmType(void) { return TYPE_AM_JDBC; }
//SQLLEN *GetStrLen(void) { return StrLen; }
	int     GetRank(void) { return Rank; }
//PVBLK   GetBlkp(void) {return Blkp;}
	void    SetCrp(PCOLRES crp) { Crp = crp; }

	// Methods
	virtual bool   SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check);
	virtual void   ReadColumn(PGLOBAL g);
	virtual void   WriteColumn(PGLOBAL g);
//void   AllocateBuffers(PGLOBAL g, int rows);
//void  *GetBuffer(DWORD rows);
//SWORD  GetBuflen(void);
	//        void   Print(PGLOBAL g, FILE *, uint);

protected:
	// Constructor used by GetMaxSize
	JDBCCOL(void);

	// Members
	//TIMESTAMP_STRUCT *Sqlbuf;    // To get SQL_TIMESTAMP's
	PCOLRES Crp;                 // To storage result
	void   *Bufp;                // To extended buffer
	PVBLK   Blkp;                // To Value Block
	//char    F_Date[12];          // Internal Date format
	PVAL    To_Val;              // To value used for Insert
//SQLLEN *StrLen;              // As returned by JDBC
//SQLLEN  Slen;                // Used with Fetch
	int     Rank;                // Rank (position) number in the query
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
	virtual AMT  GetAmType(void) {return TYPE_AM_XDBC;}

	// Methods
	//virtual int  GetRecpos(void);
	//virtual PSZ  GetFile(PGLOBAL g);
	//virtual void SetFile(PGLOBAL g, PSZ fn);
	//virtual void ResetSize(void);
	//virtual int  GetAffectedRows(void) {return AftRows;}
	//virtual PSZ  GetServer(void) {return "JDBC";}

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
}; // end of class TDBXJDC

/***********************************************************************/
/*  Used by table in source execute mode.                              */
/***********************************************************************/
class JSRCCOL : public JDBCCOL {
	friend class TDBXJDC;
public:
	// Constructors
	JSRCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PSZ am = "JDBC");

	// Implementation
	//virtual int  GetAmType(void) {return TYPE_AM_JDBC;}

	// Methods
	virtual void ReadColumn(PGLOBAL g);
	virtual void WriteColumn(PGLOBAL g);
	//        void Print(PGLOBAL g, FILE *, uint);

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
	TDBJDRV(PJDBCDEF tdp) : TDBCAT(tdp) {Maxres = tdp->Maxres; Jpath = tdp->Jpath;}

protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g);

	// Members
	int      Maxres;            // Returned lines limit
	char    *Jpath;             // Java class path
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
	virtual PQRYRES GetResult(PGLOBAL g);

	// Members
	char    *Jpath;             // Points to Java classpath
	char    *Schema;            // Points to schema name or NULL
	char    *Tab;               // Points to JDBC table name or pattern
	char    *Tabtype;           // Points to JDBC table type
	JDBCPARM Ops;               // Additional parameters
}; // end of class TDBJTB

/***********************************************************************/
/*  This is the class declaration for the columns catalog table.       */
/***********************************************************************/
class TDBJDBCL : public TDBJTB {
public:
	// Constructor
	TDBJDBCL(PJDBCDEF tdp) : TDBJTB(tdp) {}

protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g);

	// No additional Members
}; // end of class TDBJCL

#if 0
/***********************************************************************/
/*  This is the class declaration for the Data Sources catalog table.  */
/***********************************************************************/
class TDBJSRC : public TDBJDRV {
public:
	// Constructor
	TDBJSRC(PJDBCDEF tdp) : TDBJDRV(tdp) {}

protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g);

	// No additional Members
}; // end of class TDBJSRC
#endif // 0

#endif // !NJDBC
