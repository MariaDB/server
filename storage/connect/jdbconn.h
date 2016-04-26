/***********************************************************************/
/*  JDBConn.h : header file for the JDBC connection classes.           */
/***********************************************************************/
//nclude <windows.h>                           /* Windows include file */
//nclude <windowsx.h>                          /* Message crackers     */

/***********************************************************************/
/*  Included C-definition files required by the interface.             */
/***********************************************************************/
#include "block.h"

/***********************************************************************/
/*  JDBC interface.                                                    */
/***********************************************************************/
#include <jni.h>

/***********************************************************************/
/*  Constants and defines.                                             */
/***********************************************************************/
//  Miscellaneous sizing info
#define MAX_NUM_OF_MSG   10     // Max number of error messages
//efine MAX_CURRENCY     30     // Max size of Currency($) string
#define MAX_TNAME_LEN    32     // Max size of table names
//efine MAX_FNAME_LEN    256    // Max size of field names
//efine MAX_STRING_INFO  256    // Max size of string from SQLGetInfo
//efine MAX_DNAME_LEN    256    // Max size of Recordset names
#define MAX_CONNECT_LEN  512    // Max size of Connect string
//efine MAX_CURSOR_NAME  18     // Max size of a cursor name
#define DEFAULT_FIELD_TYPE 0    // TYPE_NULL

#if !defined(__WIN__)
typedef unsigned char *PUCHAR;
#endif   // !__WIN__

// Field Flags, used to indicate status of fields
//efine SQL_FIELD_FLAG_DIRTY    0x1
//efine SQL_FIELD_FLAG_NULL     0x2

// Update options flags
//efine SQL_SETPOSUPDATES       0x0001
//efine SQL_POSITIONEDSQL       0x0002
//efine SQL_GDBOUND             0x0004

enum JCATINFO {
	CAT_TAB   =     1,      // JDBC Tables
	CAT_COL   =     2,      // JDBC Columns
	CAT_KEY   =     3,      // JDBC PrimaryKeys
//CAT_STAT  =     4,      // SQLStatistics
//CAT_SPC   =     5       // SQLSpecialColumns
};

/***********************************************************************/
/*  This structure is used to control the catalog functions.           */
/***********************************************************************/
typedef struct tagJCATPARM {
	JCATINFO Id;                 // Id to indicate function 
	PQRYRES  Qrp;                // Result set pointer
	PUCHAR   DB;                 // Database (Schema)
	PUCHAR   Tab;                // Table name or pattern
	PUCHAR   Pat;                // Table type or column pattern
} JCATPARM;

// JDBC connection to a data source
class TDBJDBC;
class JDBCCOL;
class JDBConn;
class TDBXJDC;

/***********************************************************************/
/*  JDBConn class.                                                     */
/***********************************************************************/
class JDBConn : public BLOCK {
	friend class TDBJDBC;
	friend class TDBXJDC;
//friend PQRYRES GetColumnInfo(PGLOBAL, char*&, char *, int, PVBLK&);
private:
	JDBConn();                      // Standard (unused) constructor

public:
	JDBConn(PGLOBAL g, TDBJDBC *tdbp);

	int  Open(PSZ jpath, PJPARM sop);
	int  Rewind(char *sql);
	void Close(void);
	PQRYRES AllocateResult(PGLOBAL g);

	// Attributes
public:
	char *GetQuoteChar(void) { return m_IDQuoteChar; }
	// Database successfully opened?
	bool  IsOpen(void) { return m_Opened; }
//PSZ   GetStringInfo(ushort infotype);
	int   GetMaxValue(int infotype);
//PSZ   GetConnect(void) { return m_Connect; }

public:
	// Operations
	//void SetLoginTimeout(DWORD sec) {m_LoginTimeout = sec;}
	//void SetQueryTimeout(DWORD sec) {m_QueryTimeout = sec;}
	//void SetUserName(PSZ user) {m_User = user;}
	//void SetUserPwd(PSZ pwd) {m_Pwd = pwd;}
	int     GetResultSize(char *sql, JDBCCOL *colp);
	int     ExecuteQuery(char *sql);
	int     ExecuteUpdate(char *sql);
	int     Fetch(int pos = 0);
	bool    PrepareSQL(char *sql);
	int     ExecuteSQL(void);
	bool    SetParam(JDBCCOL *colp);
	int     ExecSQLcommand(char *sql);
	void    SetColumnValue(int rank, PSZ name, PVAL val);
	int     GetCatInfo(JCATPARM *cap);
	//bool  GetDataSources(PQRYRES qrp);
	bool    GetDrivers(PQRYRES qrp);
	PQRYRES GetMetaData(PGLOBAL g, char *src);

public:
	// Set special options
//void OnSetOptions(HSTMT hstmt);

	// Implementation
public:
	//virtual ~JDBConn();

	// JDBC operations
protected:
//bool Check(RETCODE rc);
//void ThrowDJX(int rc, PSZ msg/*, HSTMT hstmt = SQL_NULL_HSTMT*/);
//void ThrowDJX(PSZ msg);
//void AllocConnect(DWORD dwOptions);
//void Connect(void);
//bool DriverConnect(DWORD Options);
//void VerifyConnect(void);
//void GetConnectInfo(void);
//void Free(void);

protected:
	// Members
	PGLOBAL   m_G;
	TDBJDBC  *m_Tdb;
	JavaVM   *jvm;                      // Pointer to the JVM (Java Virtual Machine)
	JNIEnv   *env;                      // Pointer to native interface
	jclass    jdi;											// Pointer to the JdbcInterface class
	jobject   job;											// The JdbcInterface class object
	jmethodID xqid;											// The ExecuteQuery method ID
	jmethodID xuid;											// The ExecuteUpdate method ID
	jmethodID xid;											// The Execute method ID
	jmethodID grs;											// The GetResult method ID
	jmethodID readid;										// The ReadNext method ID
	jmethodID fetchid;									// The Fetch method ID
	jmethodID typid;										// The ColumnType method ID
	jmethodID prepid;										// The CreatePrepStmt method ID
	jmethodID xpid;										  // The ExecutePrep method ID
	jmethodID pcid;										  // The ClosePrepStmt method ID
	//DWORD     m_LoginTimeout;
//DWORD     m_QueryTimeout;
//DWORD     m_UpdateOptions;
	char      m_IDQuoteChar[2];
	PSZ       m_Driver;
	PSZ       m_Url;
	PSZ       m_User;
	PSZ       m_Pwd;
  int       m_Ncol;
	int       m_Aff;
	int       m_Rows;
	int       m_Fetch;
	int       m_RowsetSize;
	jboolean  m_Updatable;
	jboolean  m_Transact;
	jboolean  m_Scrollable;
	bool      m_Opened;
	bool      m_Full;
}; // end of JDBConn class definition
