/***********************************************************************/
/*  ODBC catalog function prototypes.                                  */
/***********************************************************************/
#if defined(PROMPT_OK)
char   *ODBCCheckConnection(PGLOBAL g, char *dsn, int cop);
#endif   // PROMPT_OK
PQRYRES ODBCDataSources(PGLOBAL g, int maxres, bool info);
PQRYRES ODBCColumns(PGLOBAL g, char *dsn, char *db, char *table,
                               char *colpat, int maxres, bool info);
PQRYRES ODBCSrcCols(PGLOBAL g, char *dsn, char *src);
PQRYRES ODBCTables(PGLOBAL g, char *dsn, char *db, char *tabpat,
                              int maxres, bool info);
PQRYRES ODBCDrivers(PGLOBAL g, int maxres, bool info);
