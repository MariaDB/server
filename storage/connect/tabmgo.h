/**************** tabmgo H Declares Source Code File (.H) **************/
/*  Name: tabmgo.h   Version 1.1                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2017         */
/*                                                                     */
/*  This file contains the MongoDB classes declares.                   */
/***********************************************************************/
#include "mongo.h"
#include "cmgoconn.h"

/***********************************************************************/
/*  Class used to get the columns of a mongo collection.               */
/***********************************************************************/
class MGODISC : public BLOCK {
public:
	// Constructor
	MGODISC(PGLOBAL g, int *lg);

	// Functions
	int  GetColumns(PGLOBAL g, PCSZ db, PCSZ uri, PTOS topt);
	bool FindInDoc(PGLOBAL g, bson_iter_t *iter, const bson_t *doc,
		             char *pcn, char *pfmt, int i, int k, bool b);

	// Members
	BCOL    bcol;
	PBCOL   bcp, fbcp, pbcp;
	PMGODEF tdp;
	TDBMGO *tmgp;
	int    *length;
	int     n, k, lvl;
	bool    all;
}; // end of MGODISC

/* -------------------------- TDBMGO class --------------------------- */

/***********************************************************************/
/*  This is the MongoDB Table Type class declaration.                  */
/*  The table is a collection, each record being a document.           */
/***********************************************************************/
class DllExport TDBMGO : public TDBEXT {
	friend class MGOCOL;
	friend class MGODEF;
	friend class MGODISC;
	friend PQRYRES MGOColumns(PGLOBAL, PCSZ, PCSZ, PTOS, bool);
public:
	// Constructor
	TDBMGO(MGODEF *tdp);
	TDBMGO(TDBMGO *tdbp);

	// Implementation
	virtual AMT  GetAmType(void) {return TYPE_AM_MGO;}
	virtual PTDB Duplicate(PGLOBAL g) {return (PTDB)new(g) TDBMGO(this);}

	// Methods
	virtual PTDB Clone(PTABS t);
	virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
	virtual PCOL InsertSpecialColumn(PCOL colp);
	virtual int  RowNumber(PGLOBAL g, bool b = FALSE) {return N;}

	// Database routines
	virtual int  Cardinality(PGLOBAL g);
	virtual int  GetMaxSize(PGLOBAL g);
	virtual bool OpenDB(PGLOBAL g);
	virtual int  ReadDB(PGLOBAL g);
	virtual int  WriteDB(PGLOBAL g);
	virtual int  DeleteDB(PGLOBAL g, int irc);
	virtual void CloseDB(PGLOBAL g);
	virtual bool ReadKey(PGLOBAL g, OPVAL op, const key_range *kr);

protected:
	bool Init(PGLOBAL g);

	// Members
	CMgoConn             *Cmgp;       // Points to a C Mongo connection class
	CMGOPARM							Pcg;				// Parms passed to Cmgp
	const Item           *Cnd;			  // The first condition
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
	friend class FILTER;
public:
	// Constructors
	MGOCOL(PGLOBAL g, PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i);
	MGOCOL(MGOCOL *colp, PTDB tdbp); // Constructor used in copy process

	// Implementation
	virtual int   GetAmType(void) { return Tmgp->GetAmType(); }

	// Methods
	virtual PSZ   GetJpath(PGLOBAL g, bool proj);
	virtual void  ReadColumn(PGLOBAL g);
	virtual void  WriteColumn(PGLOBAL g);

protected:
	// Default constructor not to be used
	MGOCOL(void) {}

	// Members
	TDBMGO *Tmgp;                 // To the MGO table block
	char   *Jpath;                // The json path
}; // end of class MGOCOL

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
	PTOS Topt;
	PCSZ Uri;
	PCSZ Db;
}; // end of class TDBGOL
