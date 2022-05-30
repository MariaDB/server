/***********************************************************************/
/*  JMgoConn.h : header file for the MongoDB connection classes.       */
/***********************************************************************/

/***********************************************************************/
/*  Java interface.                                                    */
/***********************************************************************/
#include "javaconn.h"

// Java connection to a MongoDB data source
class TDBJMG;
class JMGCOL;

/***********************************************************************/
/*  Include MongoDB library header files.                       	  	 */
/***********************************************************************/
typedef class JNCOL  *PJNCOL;
typedef class MGODEF *PMGODEF;
typedef class TDBJMG *PTDBJMG;
typedef class JMGCOL *PJMGCOL;

typedef struct JKCOL {
	JKCOL *Next;
	PJNCOL Jncolp;
	PCOL   Colp;
	char  *Key;
	int    N;
	bool   Array;
} *PJKC;

/***********************************************************************/
/*  Used when inserting values in a MongoDB collection.                */
/***********************************************************************/
class JNCOL : public BLOCK {
public:
	// Constructor
//JNCOL(bool ar) { Klist = NULL; Array = ar; }
	JNCOL(void) { Klist = NULL; }

	// Methods
	void AddCol(PGLOBAL g, PCOL colp, PSZ jp);

	//Members
	PJKC   Klist;
}; // end of JNCOL;

/***********************************************************************/
/*  JMgoConn class.                                                    */
/***********************************************************************/
class JMgoConn : public JAVAConn {
	friend class TDBJMG;
	friend class JMGDISC;
	//friend class TDBXJDC;
	//friend PQRYRES GetColumnInfo(PGLOBAL, char*&, char *, int, PVBLK&);
private:
	JMgoConn();                      // Standard (unused) constructor

public:
	// Constructor
	JMgoConn(PGLOBAL g, PCSZ collname, PCSZ wrapper);

	// Implementation
public:
	virtual void AddJars(PSTRG jpop, char sep);
	virtual bool Connect(PJPARM sop);
	virtual bool MakeCursor(PGLOBAL g, PTDB tdbp, PCSZ options, PCSZ filter, bool pipe);
//	PQRYRES AllocateResult(PGLOBAL g, TDBEXT *tdbp, int n);

	// Attributes
public:
//	virtual int   GetMaxValue(int infotype);

public:
	// Operations
	virtual int  Fetch(int pos = 0);
	virtual PSZ  GetColumnValue(PSZ name);

	int     CollSize(PGLOBAL g);
	bool    FindCollection(PCSZ query, PCSZ proj);
	bool    AggregateCollection(PCSZ pipeline);
	void    MakeColumnGroups(PGLOBAL g, PTDB tdbp);
	bool    GetMethodId(PGLOBAL g, MODE mode);
	jobject MakeObject(PGLOBAL g, PCOL colp, bool& error);
	jobject MakeDoc(PGLOBAL g, PJNCOL jcp);
	int     DocWrite(PGLOBAL g, PCSZ line);
	int     DocUpdate(PGLOBAL g, PTDB tdbp);
	int     DocDelete(PGLOBAL g, bool all);
	bool    Rewind(void);
	PSZ     GetDocument(void);
	bool    Stringify(PCOL colp);

protected:
	// Members
	PCSZ      CollName;									// The collation name
	jmethodID gcollid;								// The GetCollection method ID
	jmethodID countid;								  // The GetCollSize method ID
	jmethodID fcollid;									// The FindColl method ID
	jmethodID acollid;									// The AggregateColl method ID
	jmethodID readid;										// The ReadNext method ID
	jmethodID fetchid;									// The Fetch method ID
	jmethodID rewindid;									// The Rewind method ID
	jmethodID getdocid;									// The GetDoc method ID
	jmethodID objfldid;									// The ObjectField method ID
	jmethodID mkdocid;									// The MakeDocument method ID
	jmethodID mkbsonid;								  // The MakeBson method ID
	jmethodID docaddid;									// The DocAdd method ID
	jmethodID mkarid;										// The MakeArray method ID
	jmethodID araddid;									// The ArrayAdd method ID
	jmethodID insertid;									// The CollInsert method ID
	jmethodID updateid;									// The CollUpdate method ID
	jmethodID deleteid;									// The CollDelete method ID
	PJNCOL    Fpc;				              // To JNCOL classes
	int       m_Fetch;
	int       m_Ncol;
	int       m_Version;								// Java driver version (2 or 3)
}; // end of JMgoConn class definition
