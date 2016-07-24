// Timeout and net wait defaults
#define DEFAULT_LOGIN_TIMEOUT -1                  // means do not set
#define DEFAULT_QUERY_TIMEOUT -1                  // means do not set

typedef struct odbc_parms {
  char *User;                 // User connect info
  char *Pwd;                  // Password connect info
  int   Cto;                  // Connect timeout
  int   Qto;                  // Query timeout
  bool  UseCnc;               // Use SQLConnect (!SQLDriverConnect)
  } ODBCPARM, *POPARM;

/***********************************************************************/
/*  ODBC catalog function prototypes.                                  */
/***********************************************************************/
#if defined(PROMPT_OK)
char   *ODBCCheckConnection(PGLOBAL g, char *dsn, int cop);
#endif   // PROMPT_OK
PQRYRES ODBCDataSources(PGLOBAL g, int maxres, bool info);
PQRYRES ODBCColumns(PGLOBAL g, char *dsn, char *db, char *table,
                    char *colpat, int maxres, bool info, POPARM sop);
PQRYRES ODBCSrcCols(PGLOBAL g, char *dsn, char *src, POPARM sop); 
PQRYRES ODBCTables(PGLOBAL g, char *dsn, char *db, char *tabpat,
                   char *tabtyp, int maxres, bool info, POPARM sop);
PQRYRES ODBCDrivers(PGLOBAL g, int maxres, bool info);
