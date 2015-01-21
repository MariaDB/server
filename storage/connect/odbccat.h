// Timeout and net wait defaults
#define DEFAULT_LOGIN_TIMEOUT -1                  // means do not set
#define DEFAULT_QUERY_TIMEOUT -1                  // means do not set

/***********************************************************************/
/*  ODBC catalog function prototypes.                                  */
/***********************************************************************/
#if defined(PROMPT_OK)
char   *ODBCCheckConnection(PGLOBAL g, char *dsn, int cop);
#endif   // PROMPT_OK
PQRYRES ODBCDataSources(PGLOBAL g, int maxres, bool info);
PQRYRES ODBCColumns(PGLOBAL g, char *dsn, char *db, char *table,
                    char *colpat, int maxres, int cto, int qto, bool info);
PQRYRES ODBCSrcCols(PGLOBAL g, char *dsn, char *src, int cto, int qto);
PQRYRES ODBCTables(PGLOBAL g, char *dsn, char *db, char *tabpat,
                              int maxres, int cto, int qto, bool info);
PQRYRES ODBCDrivers(PGLOBAL g, int maxres, bool info);
