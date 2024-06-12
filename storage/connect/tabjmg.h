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
	bool Init(PGLOBAL g) override;
	void GetDoc(void) override {}
	bool Find(PGLOBAL g) override;

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
	AMT  GetAmType(void) override { return TYPE_AM_MGO; }
	PTDB Duplicate(PGLOBAL g) override { return (PTDB)new(g) TDBJMG(this); }

	// Methods
	PTDB Clone(PTABS t) override;
	PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
	PCOL InsertSpecialColumn(PCOL colp) override;
//virtual void SetFilter(PFIL fp);
	int  RowNumber(PGLOBAL g, bool b = FALSE) override { return N; }

	// Database routines
	int  Cardinality(PGLOBAL g) override;
	int  GetMaxSize(PGLOBAL g) override;
	bool OpenDB(PGLOBAL g) override;
	int  ReadDB(PGLOBAL g) override;
	int  WriteDB(PGLOBAL g) override;
	int  DeleteDB(PGLOBAL g, int irc) override;
	void CloseDB(PGLOBAL g) override;
	bool ReadKey(PGLOBAL g, OPVAL op, const key_range *kr) override;

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
	int   GetAmType(void) override {return Tmgp->GetAmType();}
	bool  Stringify(void) override { return Sgfy; }

	// Methods
	//virtual bool SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check);
	PSZ   GetJpath(PGLOBAL g, bool proj) override;
	void  ReadColumn(PGLOBAL g) override;
	void  WriteColumn(PGLOBAL g) override;
//bool AddValue(PGLOBAL g, bson_t *doc, char *key, bool upd);

protected:
	// Default constructor not to be used
	JMGCOL(void) = default;
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
	virtual PQRYRES GetResult(PGLOBAL g) override;

	// Members
	PTOS Topt;
	PCSZ Uri;
	PCSZ Db;
}; // end of class TDBGOL
