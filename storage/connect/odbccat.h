/***********************************************************************/
/*  ODBC catalog function prototypes.                                  */
/***********************************************************************/
PQRYRES ODBCDataSources(PGLOBAL g, bool info);
PQRYRES ODBCColumns(PGLOBAL g, char *dsn, char *table,
                                          char *colpat, bool info);
PQRYRES ODBCTables(PGLOBAL g, char *dsn, char *tabpat, bool info);
PQRYRES ODBCDrivers(PGLOBAL g, bool info);
