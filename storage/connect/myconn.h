/***********************************************************************/
/*  MYCONN.H     Olivier Bertrand    2007-2013                         */
/*                                                                     */
/*  This is the declaration file for the MySQL connection class and    */
/*  a few utility functions used to communicate with MySQL.            */
/*                                                                     */
/*  DO NOT define DLL_EXPORT in your application so these items are    */
/*  declared are imported from the Myconn DLL.                         */
/***********************************************************************/
#if defined(_WIN32)
#include <winsock.h>
#else   // !_WIN32
#include <sys/socket.h>
#endif  // !_WIN32
#include <mysql.h>
#include <errmsg.h>
#include "myutil.h"

#if defined(_WIN32) && defined(MYCONN_EXPORTS)
#if defined(DLL_EXPORT)
#define DllItem _declspec(dllexport)
#else   // !DLL_EXPORT
#define DllItem _declspec(dllimport)
#endif  // !DLL_EXPORT
#else   // !_WIN32  ||        !MYCONN_EXPORTS
#define DllItem
#endif  // !_WIN32

#define MYSQL_ENABLED  0x00000001
#define MYSQL_LOGON    0x00000002

typedef class MYSQLC *PMYC;

/***********************************************************************/
/*  Prototypes of info functions.                                      */
/***********************************************************************/
PQRYRES MyColumns(PGLOBAL g, THD *thd, const char *host, const char *db,
                  const char *user, const char *pwd,
                  const char *table, const char *colpat,
                  int port, bool info);

PQRYRES SrcColumns(PGLOBAL g, const char *host, const char *db,
                   const char *user, const char *pwd,
                   const char *srcdef, int port);

uint GetDefaultPort(void);

/* -------------------------- MYCONN class --------------------------- */

/***********************************************************************/
/*  MYSQLC exported/imported class. A MySQL connection.                */
/***********************************************************************/
class DllItem MYSQLC {
  friend class TDBMYSQL;
  friend class MYSQLCOL;
  friend class TDBMYEXC;
  // Construction
 public:
  MYSQLC(void);

  // Implementation
  int    GetRows(void) {return m_Rows;}
  bool   Connected(void);

  // Methods
  int     GetResultSize(PGLOBAL g, PSZ sql);
  int     GetTableSize(PGLOBAL g, PSZ query);
  int     Open(PGLOBAL g, const char *host, const char *db,
                          const char *user= "root", const char *pwd= "*",
                          int pt= 0, const char *csname = NULL);
  int     KillQuery(ulong id);
  int     ExecSQL(PGLOBAL g, const char *query, int *w = NULL);
  int     ExecSQLcmd(PGLOBAL g, const char *query, int *w);
#if defined(MYSQL_PREPARED_STATEMENTS)
  int     PrepareSQL(PGLOBAL g, const char *query);
  int     ExecStmt(PGLOBAL g);
  int     BindParams(PGLOBAL g, MYSQL_BIND *bind);
#endif   // MYSQL_PREPARED_STATEMENTS
  PQRYRES GetResult(PGLOBAL g, bool pdb = FALSE);
  int     Fetch(PGLOBAL g, int pos);
  char   *GetCharField(int i);
  int     GetFieldLength(int i);
  int     Rewind(PGLOBAL g, PSZ sql);
  void    FreeResult(void);
  void    Close(void);

 protected:
  MYSQL_FIELD *GetNextField(void);
  void    DataSeek(my_ulonglong row);

  // Members
  MYSQL      *m_DB;         // The return from MySQL connection
#if defined (MYSQL_PREPARED_STATEMENTS)
	MYSQL_STMT *m_Stmt;       // Prepared statement handle
#endif    // MYSQL_PREPARED_STATEMENTS
	MYSQL_RES  *m_Res;        // Points to MySQL Result
  MYSQL_ROW   m_Row;        // Point to current row
  int         m_Rows;       // The number of rows of the result
  int         N;
  int         m_Fields;     // The number of result fields
  int         m_Afrw;       // The number of affected rows
  bool        m_Use;        // Use or store result set
  const char *csname;       // Table charset name
  }; // end of class MYSQLC

