// Timeout and net wait defaults
#define DEFAULT_LOGIN_TIMEOUT -1                  // means do not set
#define DEFAULT_QUERY_TIMEOUT -1                  // means do not set

typedef struct odbc_parms {
  PCSZ User;                 // User connect info
	PCSZ Pwd;                  // Password connect info
  int  Cto;                  // Connect timeout
  int  Qto;                  // Query timeout
  bool UseCnc;               // Use SQLConnect (!SQLDriverConnect)
  } ODBCPARM, *POPARM;

/***********************************************************************/
/*  ODBC catalog function prototypes.                                  */
/***********************************************************************/
#if defined(PROMPT_OK)
char   *ODBCCheckConnection(PGLOBAL g, char *dsn, int cop);
#endif   // PROMPT_OK
PQRYRES ODBCDataSources(PGLOBAL g, int maxres, bool info);
PQRYRES ODBCColumns(PGLOBAL g, PCSZ dsn, PCSZ db, PCSZ table,
	                  PCSZ colpat, int maxres, bool info, POPARM sop);
PQRYRES ODBCSrcCols(PGLOBAL g, char *dsn, char *src, POPARM sop); 
PQRYRES ODBCTables(PGLOBAL g, PCSZ dsn, PCSZ db, PCSZ tabpat,
	                 PCSZ tabtyp, int maxres, bool info, POPARM sop);
PQRYRES ODBCDrivers(PGLOBAL g, int maxres, bool info);
