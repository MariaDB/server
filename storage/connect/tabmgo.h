/**************** tabmgo H Declares Source Code File (.H) **************/
/*  Name: tabmgo.h   Version 1.0                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2017         */
/*                                                                     */
/*  This file contains the MongoDB classes declares.                   */
/***********************************************************************/
#include "osutil.h"
#include "block.h"
#include "colblk.h"

/***********************************************************************/
/*  Include MongoDB library header files.                       	  	 */
/***********************************************************************/
#include <bson.h>
#include <bcon.h>
#include <mongoc.h>

typedef class MGODEF *PMGODEF;
typedef class TDBMGO *PTDBMGO;
typedef class MGOCOL *PMGOCOL;

/***********************************************************************/
/*  MongoDB table.                                                     */
/***********************************************************************/
class DllExport MGODEF : public EXTDEF {          /* Table description */
	friend class TDBMGO;
	friend class MGOFAM;
public:
	// Constructor
	MGODEF(void);

	// Implementation
	virtual const char *GetType(void) { return "MONGO"; }

	// Methods
	virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
	virtual PTDB GetTable(PGLOBAL g, MODE m);

protected:
	// Members
	const char *Uri;							/* MongoDB connection URI              */
	char *Colist;                 /* Options list                        */
	char *Filter;									/* Filtering query                     */						
	int   Level;                  /* Used for catalog table              */
	int   Base;                   /* The array index base                */
}; // end of MGODEF

/* -------------------------- TDBMGO class --------------------------- */

/***********************************************************************/
/*  This is the MongoDB Access Method class declaration.               */
/*  The table is a collection, each record being a document.           */
/***********************************************************************/
class DllExport TDBMGO : public TDBEXT {
	friend class MGOCOL;
	friend class MGODEF;
public:
	// Constructor
	TDBMGO(PMGODEF tdp);
	TDBMGO(TDBMGO *tdbp);

	// Implementation
	virtual AMT   GetAmType(void) { return TYPE_AM_MGO; }
	virtual PTDB  Duplicate(PGLOBAL g) { return (PTDB)new(g) TDBMGO(this); }

	// Methods
	virtual PTDB  Clone(PTABS t);
	virtual PCOL  MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
	virtual PCOL  InsertSpecialColumn(PCOL colp);
	virtual int   RowNumber(PGLOBAL g, bool b = FALSE) {return N;}

	// Database routines
	virtual int   Cardinality(PGLOBAL g);
	virtual int   GetMaxSize(PGLOBAL g);
	virtual bool  OpenDB(PGLOBAL g);
	virtual int   ReadDB(PGLOBAL g);
	virtual int   WriteDB(PGLOBAL g);
	virtual int   DeleteDB(PGLOBAL g, int irc);
	virtual void  CloseDB(PGLOBAL g);
	virtual bool  ReadKey(PGLOBAL g, OPVAL op, const key_range *kr);

protected:
	bool  Init(PGLOBAL g);
	void  ShowDocument(bson_iter_t *i, const bson_t *b, const char *k);

	// Members
	mongoc_uri_t         *Uri;
	mongoc_client_pool_t *Pool;				// Thread safe client pool
	mongoc_client_t      *Client;		  // The MongoDB client
	mongoc_database_t    *Database;	  // The MongoDB database
	mongoc_collection_t  *Collection; // The MongoDB collection
	mongoc_cursor_t      *Cursor;
	const bson_t         *Document;
	bson_t               *Query;			// MongoDB cursor filter
	bson_t               *Opts;			  // MongoDB cursor options
	bson_error_t          Error;
	const char           *Uristr;
	const char           *Db_name;
	const char           *Coll_name;
	const char           *Options;		// The MongoDB options
	const char           *Filter;			// The filtering query
	int                   Fpos;       // The current row index
	int                   N;          // The current Rownum
	int                   B;          // Array index base
	bool                  Done;			  // Init done
}; // end of class TDBMGO

/* --------------------------- MGOCOL class -------------------------- */

/***********************************************************************/
/*  Class MGOCOL: MongoDB access method column descriptor.             */
/***********************************************************************/
class DllExport MGOCOL : public EXTCOL {
	friend class TDBMGO;
public:
	// Constructors
	MGOCOL(PGLOBAL g, PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i);
	MGOCOL(MGOCOL *colp, PTDB tdbp); // Constructor used in copy process

	// Implementation
	virtual int  GetAmType(void) { return Tmgp->GetAmType(); }

	// Methods
	virtual bool SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check);
	virtual void ReadColumn(PGLOBAL g);
	virtual void WriteColumn(PGLOBAL g);

protected:
	// Default constructor not to be used
	MGOCOL(void) {}

	// Members
	TDBMGO *Tmgp;                 // To the MGO table block
	bson_iter_t Iter;						  // Used to retrieve column value
	bson_iter_t Desc;						  // Descendant iter
	char   *Jpath;                // The json path
}; // end of class MGOCOL

#if 0
/***********************************************************************/
/*  This is the class declaration for the MONGO catalog table.         */
/***********************************************************************/
class DllExport TDBGOL : public TDBCAT {
public:
	// Constructor
	TDBGOL(PMGODEF tdp);

protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g);

	// Members
	PTOS  Topt;
	char *Db;
	char *Dsn;
}; // end of class TDBGOL
#endif // 0
