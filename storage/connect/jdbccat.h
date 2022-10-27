#ifndef __JDBCCAT_H
#define __JDBCCAT_H

// Timeout and net wait defaults
#define DEFAULT_LOGIN_TIMEOUT -1                  // means do not set
#define DEFAULT_QUERY_TIMEOUT -1                  // means do not set

typedef struct jdbc_parms {
	int CheckSize(int rows);
	PCSZ  Driver;               // JDBC driver
	PCSZ  Url;                  // Driver URL
	PCSZ  User;                 // User connect info
	PCSZ  Pwd;                  // Password connect info
//int   Cto;                  // Connect timeout
//int   Qto;                  // Query timeout
	int   Version;							// Driver version
	int   Fsize;								// Fetch size
	bool  Scrollable;						// Scrollable cursor
} JDBCPARM, *PJPARM;

/***********************************************************************/
/*  JDBC catalog function prototypes.                                  */
/***********************************************************************/
#if defined(PROMPT_OK)
char   *JDBCCheckConnection(PGLOBAL g, PCSZ dsn, int cop);
#endif   // PROMPT_OK
//PQRYRES JDBCDataSources(PGLOBAL g, int maxres, bool info);
PQRYRES JDBCColumns(PGLOBAL g, PCSZ db, PCSZ table,
	PCSZ colpat, int maxres, bool info, PJPARM sop);
PQRYRES JDBCSrcCols(PGLOBAL g, PCSZ src, PJPARM sop);
PQRYRES JDBCTables(PGLOBAL g, PCSZ db, PCSZ tabpat,
	PCSZ tabtyp, int maxres, bool info, PJPARM sop);
PQRYRES JDBCDrivers(PGLOBAL g, int maxres, bool info);

#endif // __JDBCCAT_H
