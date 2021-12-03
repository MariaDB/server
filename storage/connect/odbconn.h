/***********************************************************************/
/*  ODBConn.h : header file for the ODBC connection classes.           */
/***********************************************************************/
//nclude <windows.h>                           /* Windows include file */
//nclude <windowsx.h>                          /* Message crackers     */

/***********************************************************************/
/*  Included C-definition files required by the interface.             */
/***********************************************************************/
#include "block.h"

/***********************************************************************/
/*  ODBC interface.                                                    */
/***********************************************************************/
#include <sql.h>
#include <sqlext.h>

/***********************************************************************/
/*  Constants and defines.                                             */
/***********************************************************************/
//  Miscellaneous sizing info
#define MAX_NUM_OF_MSG   10     // Max number of error messages
//efine MAX_CURRENCY     30     // Max size of Currency($) string
#define MAX_TNAME_LEN    32     // Max size of table names
//efine MAX_FNAME_LEN    256    // Max size of field names
#define MAX_STRING_INFO  256    // Max size of string from SQLGetInfo
//efine MAX_DNAME_LEN    256    // Max size of Recordset names
#define MAX_CONNECT_LEN  1024   // Max size of Connect string
//efine MAX_CURSOR_NAME  18     // Max size of a cursor name
//efine DEFAULT_FIELD_TYPE SQL_TYPE_NULL // pick "C" data type to match SQL data type

#if !defined(_WIN32)
typedef unsigned char *PUCHAR;
#endif   // !_WIN32

// Field Flags, used to indicate status of fields
//efine SQL_FIELD_FLAG_DIRTY    0x1
//efine SQL_FIELD_FLAG_NULL     0x2

// Update options flags
#define SQL_SETPOSUPDATES       0x0001
#define SQL_POSITIONEDSQL       0x0002
//efine SQL_GDBOUND             0x0004

enum CATINFO {CAT_TAB   =     1,      /* SQLTables                     */
              CAT_COL   =     2,      /* SQLColumns                    */
              CAT_KEY   =     3,      /* SQLPrimaryKeys                */
              CAT_STAT  =     4,      /* SQLStatistics                 */
              CAT_SPC   =     5};     /* SQLSpecialColumns             */

/***********************************************************************/
/*  This structure is used to control the catalog functions.           */  
/***********************************************************************/
typedef struct tagCATPARM { 
  CATINFO  Id;                 // Id to indicate function 
  PQRYRES  Qrp;                // Result set pointer
	PCSZ     DB;                 // Database (Schema)
	PCSZ     Tab;                // Table name or pattern
	PCSZ     Pat;                // Table type or column pattern
  SQLLEN* *Vlen;               // To array of indicator values
  UWORD   *Status;             // To status block
  // For SQLStatistics
  UWORD    Unique;             // Index type
  UWORD    Accuracy;           // For Cardinality and Pages
  // For SQLSpecialColumns 
  UWORD    ColType;
  UWORD    Scope; 
  UWORD    Nullable;
  } CATPARM; 
   
// ODBC connection to a data source
class TDBODBC;
class ODBCCOL;
class ODBConn;

/***********************************************************************/
/*  Class DBX (ODBC exception).                                        */
/***********************************************************************/
class DBX : public BLOCK {
  friend class ODBConn;
  // Construction (by ThrowDBX only) -- destruction
 protected:
  DBX(RETCODE rc, PCSZ msg = NULL);
 public:
//virtual ~DBX() {}
//void operator delete(void*, PGLOBAL, void*) {};

  // Implementation (use ThrowDBX to create)
  RETCODE GetRC(void) {return m_RC;}
  PCSZ    GetMsg(void) {return m_Msg;}
  PCSZ    GetErrorMessage(int i);

 protected:
  bool    BuildErrorMessage(ODBConn* pdb, HSTMT hstmt = SQL_NULL_HSTMT);

  // Attributes
  RETCODE m_RC;
  PCSZ    m_Msg;
  PCSZ    m_ErrMsg[MAX_NUM_OF_MSG];
  }; // end of DBX class definition

/***********************************************************************/
/*  ODBConn class.                                                     */
/***********************************************************************/
class ODBConn : public BLOCK {
  friend class TDBODBC;
  friend class DBX;
//friend PQRYRES GetColumnInfo(PGLOBAL, char*&, char *, int, PVBLK&);
 private:
  ODBConn();                      // Standard (unused) constructor

 public:
  ODBConn(PGLOBAL g, TDBODBC *tdbp);

  enum DOP {                      // Db Open oPtions
    traceSQL =        0x0001,     // Trace SQL calls
    openReadOnly =    0x0002,     // Open database read only
    useCursorLib =    0x0004,     // Use ODBC cursor lib
    noOdbcDialog =    0x0008,     // Don't display ODBC Connect dialog
    forceOdbcDialog = 0x0010};    // Always display ODBC connect dialog

  int  Open(PCSZ ConnectString, POPARM sop, DWORD Options = 0);
  int  Rewind(char *sql, ODBCCOL *tocols);
  void Close(void);
  PQRYRES AllocateResult(PGLOBAL g);

  // Attributes
 public:
  char *GetQuoteChar(void) {return m_IDQuoteChar;}
  // Database successfully opened?
  bool  IsOpen(void) {return m_hdbc != SQL_NULL_HDBC;}
  PSZ   GetStringInfo(ushort infotype);
  int   GetMaxValue(ushort infotype);
  PCSZ  GetConnect(void) {return m_Connect;}

 public:
  // Operations
//void SetLoginTimeout(DWORD sec) {m_LoginTimeout = sec;}
//void SetQueryTimeout(DWORD sec) {m_QueryTimeout = sec;}
//void SetUserName(PSZ user) {m_User = user;}
//void SetUserPwd(PSZ pwd) {m_Pwd = pwd;}
  int  GetResultSize(char *sql, ODBCCOL *colp);
  int  ExecDirectSQL(char *sql, ODBCCOL *tocols);
  int  Fetch(int pos = 0);
  int  PrepareSQL(char *sql);
  int  ExecuteSQL(void);
  bool BindParam(ODBCCOL *colp);
  bool ExecSQLcommand(char *sql);
  int  GetCatInfo(CATPARM *cap);
  bool GetDataSources(PQRYRES qrp);
  bool GetDrivers(PQRYRES qrp);
  PQRYRES GetMetaData(PGLOBAL g, PCSZ dsn, PCSZ src);

 public:
  // Set special options
  void OnSetOptions(HSTMT hstmt);

  // Implementation
 public:
//virtual ~ODBConn();

  // ODBC operations
 protected:
  bool Check(RETCODE rc);
  void ThrowDBX(RETCODE rc, PCSZ msg, HSTMT hstmt = SQL_NULL_HSTMT);
  void ThrowDBX(PCSZ msg);
  void AllocConnect(DWORD dwOptions);
  void Connect(void);
  bool DriverConnect(DWORD Options);
  void VerifyConnect(void);
  void GetConnectInfo(void);
//void Free(void);

 protected:
  // Static members
//static HENV m_henv;
//static int  m_nAlloc;            // per-Appl reference to HENV above

  // Members
  PGLOBAL  m_G;
  TDBODBC *m_Tdb;
  HENV     m_henv;
  HDBC     m_hdbc;
  HSTMT    m_hstmt;
  DWORD    m_LoginTimeout;
  DWORD    m_QueryTimeout;
  DWORD    m_UpdateOptions;
  DWORD    m_RowsetSize;
  char     m_IDQuoteChar[2];
	PFBLOCK  m_Fp;
	PCSZ     m_Connect;
  PCSZ     m_User;
  PCSZ     m_Pwd;
  int      m_Catver;
  int      m_Rows;
  int      m_Fetch;
  bool     m_Updatable;
  bool     m_Transact;
  bool     m_Scrollable;
  bool     m_UseCnc;
  bool     m_Full;
  }; // end of ODBConn class definition
