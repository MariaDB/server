/***********************************************************************/
/*  ODBC catalog function prototypes.                                  */
/***********************************************************************/
PQRYRES ODBCDataSources(PGLOBAL g, bool info);
PQRYRES MyODBCCols(PGLOBAL g, char *dsn, char *tab, bool info);
PQRYRES ODBCTables(PGLOBAL g, char *dsn, char *tabpat, bool info);
PQRYRES ODBCDrivers(PGLOBAL g, bool info);
