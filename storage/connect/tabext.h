/*************** Tabext H Declares Source Code File (.H) ***************/
/*  Name: TABEXT.H  Version 1.1                                        */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2017 - 2019  */
/*                                                                     */
/*  This is the EXTDEF, TABEXT and EXTCOL classes definitions.         */
/***********************************************************************/

#ifndef __TABEXT_H
#define __TABEXT_H

#include "reldef.h"

typedef class ALIAS *PAL;

class ALIAS : public BLOCK {
 public:
	ALIAS(PAL x, PSZ n, PSZ a, bool h)
	     {Next = x, Name = n, Alias = a, Having = h;}

	PAL  Next;
	PSZ  Name;
	PSZ  Alias;
	bool Having;
}; // end of class ALIAS

// Condition filter structure
class CONDFIL : public BLOCK {
 public:
	// Constructor
	CONDFIL(uint idx, AMT type);

	// Functions
	int Init(PGLOBAL g, PHC hc);
	const char *Chk(const char *cln, bool *h);

	// Members
//const Item *Cond;
	AMT   Type;
	uint  Idx;
	OPVAL Op;
	PCMD  Cmds;
	PAL   Alist;
	bool  All;
	bool  Bd;
	bool  Hv;
	char *Body;
	char *Having;
}; // end of class CONDFIL

/***********************************************************************/
/*  This class corresponds to the data base description for external   */
/*  tables of type MYSQL, ODBC, JDBC...                                */
/***********************************************************************/
class DllExport EXTDEF : public TABDEF {                  /* EXT table */
	friend class TDBEXT;
public:
	// Constructor
	EXTDEF(void);                  // Constructor

	// Implementation
	virtual const char *GetType(void) { return "EXT"; }
	inline PCSZ GetTabname(void) { return Tabname; }
	inline PCSZ GetTabschema(void) { return Tabschema; }
	inline PCSZ GetUsername(void) { return Username; };
	inline PCSZ GetPassword(void) { return Password; };
	inline PSZ  GetTabcat(void) { return Tabcat; }
	inline PSZ  GetSrcdef(void) { return Srcdef; }
	inline char GetSep(void) { return (Sep) ? *Sep : 0; }
	inline int  GetQuoted(void) { return Quoted; }
	inline int  GetOptions(void) { return Options; }

	// Methods
	virtual int  Indexable(void) { return 2; }
	virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);

protected:
	// Members
	PCSZ    Tabname;              /* External table name                 */
	PCSZ    Tabschema;            /* External table schema               */
	PCSZ    Username;             /* User connect name                   */
	PCSZ    Password;             /* Password connect info               */
	PSZ     Tabcat;               /* External table catalog              */
	PSZ     Tabtyp;               /* Catalog table type                  */
	PSZ     Colpat;               /* Catalog column pattern              */
	PSZ     Srcdef;               /* The source table SQL definition     */
	PSZ     Qchar;                /* Identifier quoting character        */
	PSZ     Qrystr;               /* The original query                  */
	PSZ     Sep;                  /* Decimal separator                   */
//PSZ     Alias;                /* Column alias list                   */
	PSZ     Phpos;                /* Place holer positions               */
	int     Options;              /* Open connection options             */
	int     Cto;                  /* Open connection timeout             */
	int     Qto;                  /* Query (command) timeout             */
	int     Quoted;               /* Identifier quoting level            */
	int     Maxerr;               /* Maxerr for an Exec table            */
	int     Maxres;               /* Maxres for a catalog table          */
	int     Memory;               /* Put result set in memory            */
	bool    Scrollable;           /* Use scrollable cursor               */
	bool    Xsrc;                 /* Execution type                      */
}; // end of EXTDEF

/***********************************************************************/
/*  This is the base class for all external tables.                    */
/***********************************************************************/
class DllExport TDBEXT : public TDB {
	friend class JAVAConn;
	friend class JMgoConn;
public:
	// Constructors
	TDBEXT(EXTDEF *tdp);
	TDBEXT(PTDBEXT tdbp);

	// Implementation

	// Properties
	virtual bool IsRemote(void) { return true; }

	// Methods
	virtual PCSZ GetServer(void) { return "Remote"; }
	virtual int  GetRecpos(void);

	// Database routines
	virtual int  GetMaxSize(PGLOBAL g);
	virtual int  GetProgMax(PGLOBAL g);

protected:
	// Internal functions
	virtual bool MakeSrcdef(PGLOBAL g);
	virtual bool MakeSQL(PGLOBAL g, bool cnt);
	//virtual bool MakeInsert(PGLOBAL g);
	virtual bool MakeCommand(PGLOBAL g);
	void RemoveConst(PGLOBAL g, char *stmt);
	int Decode(PCSZ utf, char *buf, size_t n);

	// Members
	PQRYRES Qrp;                // Points to storage result
	PSTRG   Query;              // Constructed SQL query
	PCSZ    TableName;          // Points to ODBC table name
	PCSZ    Schema;             // Points to ODBC table Schema
	PCSZ    User;               // User connect info
	PCSZ    Pwd;                // Password connect info
	char   *Catalog;            // Points to ODBC table Catalog
	char   *Srcdef;             // The source table SQL definition
	char   *Count;              // Points to count(*) SQL statement
 //char   *Where;              // Points to local where clause
	char   *Quote;              // The identifier quoting character
	char   *MulConn;            // Used for multiple ODBC tables
	char   *DBQ;                // The address part of Connect string
	char   *Qrystr;             // The original query
	char    Sep;                // The decimal separator
	int     Options;            // Connect options
	int     Cto;                // Connect timeout
	int     Qto;                // Query timeout
	int     Quoted;             // The identifier quoting level
	int     Fpos;               // Position of last read record
	int     Curpos;             // Cursor position of last fetch
	int     AftRows;            // The number of affected rows
	int     Rows;               // Rowset size
	int     CurNum;             // Current buffer line number
	int     Rbuf;               // Number of lines read in buffer
	int     BufSize;            // Size of connect string buffer
	int     Nparm;              // The number of statement parameters
	int     Memory;             // 0: No 1: Alloc 2: Put 3: Get
	int     Ncol;							  // The column number (JDBC)
	bool    Scrollable;         // Use scrollable cursor
	bool    Placed;             // True for position reading
}; // end of class TDBEXT

/***********************************************************************/
/*  Virtual class EXTCOL: external column.                             */
/***********************************************************************/
class DllExport EXTCOL : public COLBLK {
	friend class TDBEXT;
public:
	// Constructor
	EXTCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am);
	EXTCOL(PEXTCOL colp, PTDB tdbp); // Constructor used in copy process

	// Implementation
	inline int   GetRank(void) { return Rank; }
	inline void  SetRank(int k) { Rank = k; }
	//inline PVBLK GetBlkp(void) {return Blkp;}
	inline void  SetCrp(PCOLRES crp) { Crp = crp; }

	// Methods
	virtual bool   SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check);
	virtual void   ReadColumn(PGLOBAL) = 0;
	virtual void   WriteColumn(PGLOBAL) = 0;

protected:
	// Constructor for count(*) column
	EXTCOL(void);

	// Members
	PCOLRES Crp;                 // To storage result
	void   *Bufp;                // To extended buffer
	PVBLK   Blkp;                // To Value Block
	PVAL    To_Val;              // To value used for Insert
	int     Rank;                // Rank (position) number in the query
	//int     Flag;								 // ???
}; // end of class EXTCOL

#endif // __TABEXT_H
