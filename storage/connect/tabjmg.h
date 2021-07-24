/**************** tabjmg H Declares Source Code File (.H) **************/
/*  Name: tabjmg.h   Version 1.3                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2017 - 2021  */
/*                                                                     */
/*  This file contains the MongoDB classes using the Java Driver.      */
/***********************************************************************/
#include "mongo.h"
#include "jmgoconn.h"
#include "jdbccat.h"

/***********************************************************************/
/*  Class used to get the columns of a mongo collection.               */
/***********************************************************************/
class JMGDISC : public MGODISC {
public:
	// Constructor
	JMGDISC(PGLOBAL g, int *lg);

	// Methods
	virtual bool Init(PGLOBAL g);
	virtual void GetDoc(void) {}
	virtual bool Find(PGLOBAL g);

protected:
	// Function
	bool ColDesc(PGLOBAL g, jobject obj, char *pcn, char *pfmt, 
							 int ncol, int k);

	// Members
	JMgoConn *Jcp;                // Points to a Mongo connection class
	jmethodID columnid;						// The ColumnDesc method ID
	jmethodID bvnameid;						// The ColDescName method ID
}; // end of JMGDISC

/* -------------------------- TDBJMG class --------------------------- */

/***********************************************************************/
/*  This is the MongoDB Table Type using the Java Driver.              */
/*  The table is a collection, each record being a document.           */
/***********************************************************************/
class DllExport TDBJMG : public TDBEXT {
	friend class JMGCOL;
	friend class MGODEF;
	friend class JMGDISC;
	friend class JAVAConn;
	friend PQRYRES MGOColumns(PGLOBAL, PCSZ, PCSZ, PTOS, bool);
public:
	// Constructor
	TDBJMG(PMGODEF tdp);
	TDBJMG(TDBJMG *tdbp);

	// Implementation
	virtual AMT  GetAmType(void) { return TYPE_AM_MGO; }
	virtual PTDB Duplicate(PGLOBAL g) { return (PTDB)new(g) TDBJMG(this); }

	// Methods
	virtual PTDB Clone(PTABS t);
	virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
	virtual PCOL InsertSpecialColumn(PCOL colp);
//virtual void SetFilter(PFIL fp);
	virtual int  RowNumber(PGLOBAL g, bool b = FALSE) { return N; }

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
	JMgoConn  *Jcp;                // Points to a Mongo connection class
//JMGCOL    *Cnp;                // Points to count(*) column
	JDBCPARM   Ops;                // Additional parameters
	PCSZ       Uri;
	PCSZ       Db_name;
	PCSZ       Coll_name;
	PCSZ       Options;		         // The MongoDB options
	PCSZ       Filter;			       // The filtering query
	PCSZ			 Strfy;			         // The stringified columns
	PSZ        Wrapname;           // Java wrapper name
	int        Fpos;               // The current row index
	int        N;                  // The current Rownum
	int        B;                  // Array index base
	bool       Done;			         // Init done
	bool       Pipe;			         // True for pipeline
}; // end of class TDBJMG

/* --------------------------- JMGCOL class -------------------------- */

/***********************************************************************/
/*  Class JMGCOL: MongoDB access method column descriptor.             */
/***********************************************************************/
class DllExport JMGCOL : public EXTCOL {
	friend class TDBJMG;
	friend class FILTER;
public:
	// Constructors
	JMGCOL(PGLOBAL g, PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i);
	JMGCOL(JMGCOL *colp, PTDB tdbp); // Constructor used in copy process

	// Implementation
	virtual int   GetAmType(void) {return Tmgp->GetAmType();}
	virtual bool  Stringify(void) { return Sgfy; }

	// Methods
	//virtual bool SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check);
	virtual PSZ   GetJpath(PGLOBAL g, bool proj);
	virtual void  ReadColumn(PGLOBAL g);
	virtual void  WriteColumn(PGLOBAL g);
//bool AddValue(PGLOBAL g, bson_t *doc, char *key, bool upd);

protected:
	// Default constructor not to be used
	JMGCOL(void) {}
//char *GetProjPath(PGLOBAL g);
//char *Mini(PGLOBAL g, const bson_t *bson, bool b);

	// Members
	TDBJMG *Tmgp;                 // To the MGO table block
	char   *Jpath;                // The json path
	bool    Sgfy;									// True if stringified
}; // end of class JMGCOL

/***********************************************************************/
/*  This is the class declaration for the MONGO catalog table.         */
/***********************************************************************/
class DllExport TDBJGL : public TDBCAT {
public:
	// Constructor
	TDBJGL(PMGODEF tdp);

protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g);

	// Members
	PTOS Topt;
	PCSZ Uri;
	PCSZ Db;
}; // end of class TDBGOL
