/***********************************************************************/
/*  JDBConn.h : header file for the JDBC connection classes.           */
/***********************************************************************/
#include "javaconn.h"

// JDBC connection to a data source
class TDBJDBC;
class JDBCCOL;
class JDBConn;
class TDBXJDC;

/***********************************************************************/
/*  JDBConn class.                                                     */
/***********************************************************************/
class JDBConn : public JAVAConn {
	friend class TDBJDBC;
	friend class TDBXJDC;
//friend PQRYRES GetColumnInfo(PGLOBAL, char*&, char *, int, PVBLK&);
private:
	JDBConn();                      // Standard (unused) constructor

public:
	// Constructor
	JDBConn(PGLOBAL g, PCSZ wrapper);

	virtual void AddJars(PSTRG jpop, char sep);
	PQRYRES AllocateResult(PGLOBAL g, PTDB tdbp);

	// Attributes
public:
	char   *GetQuoteChar(void) { return m_IDQuoteChar; }
	bool    SetUUID(PGLOBAL g, PTDBJDBC tjp);
	virtual int  GetMaxValue(int infotype);

public:
	// Operations
	virtual bool Connect(PJPARM sop);
	virtual bool MakeCursor(PGLOBAL g, PTDB tdbp, PCSZ options,
		PCSZ filter, bool pipe) {return true;}
	virtual int  GetResultSize(PCSZ sql, PCOL colp);
	virtual int  ExecuteCommand(PCSZ sql);
	virtual int  ExecuteQuery(PCSZ sql);
	virtual int  ExecuteUpdate(PCSZ sql);
	virtual int  Fetch(int pos = 0);
	virtual void SetColumnValue(int rank, PSZ name, PVAL val);

	// Jdbc operations
	bool    PrepareSQL(PCSZ sql);
	int     ExecuteSQL(void);					 // Prepared statement
	bool    SetParam(JDBCCOL *colp);
	int     GetCatInfo(JCATPARM *cap);
	bool    GetDrivers(PQRYRES qrp);
	PQRYRES GetMetaData(PGLOBAL g, PCSZ src);
	int     Rewind(PCSZ sql);

	// Implementation
public:
	//virtual ~JDBConn();

protected:
	// Members
	jmethodID xqid;							// The ExecuteQuery method ID
	jmethodID xuid;							// The ExecuteUpdate method ID
	jmethodID xid;							// The Execute method ID
	jmethodID grs;							// The GetResult method ID
	jmethodID readid;						// The ReadNext method ID
	jmethodID fetchid;					// The Fetch method ID
	jmethodID typid;						// The ColumnType method ID
	jmethodID prepid;						// The CreatePrepStmt method ID
	jmethodID xpid;							// The ExecutePrep method ID
	jmethodID pcid;							// The ClosePrepStmt method ID
	jmethodID objfldid;					// The ObjectField method ID
	jmethodID chrfldid;					// The StringField method ID
	jmethodID intfldid;					// The IntField method ID
	jmethodID dblfldid;					// The DoubleField method ID
	jmethodID fltfldid;					// The FloatField method ID
	jmethodID datfldid;					// The DateField method ID
	jmethodID timfldid;					// The TimeField method ID
	jmethodID tspfldid;					// The TimestampField method ID
	jmethodID bigfldid;					// The BigintField method ID
	jmethodID uidfldid;					// The UuidField method ID
	char      m_IDQuoteChar[2];
	PCSZ      m_Pwd;
  int       m_Ncol;
	int       m_Aff;
	int       m_Fetch;
	int       m_RowsetSize;
	jboolean  m_Updatable;
	jboolean  m_Transact;
	jboolean  m_Scrollable;
	bool      m_Full;
}; // end of JDBConn class definition
