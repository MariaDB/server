// Timeout and net wait defaults
#define DEFAULT_LOGIN_TIMEOUT -1                  // means do not set
#define DEFAULT_QUERY_TIMEOUT -1                  // means do not set

typedef struct jdbc_parms {
	int   CheckSize(int rows);
	char *Driver;               // JDBC driver
	char *Url;                  // Driver URL
	char *User;                 // User connect info
	char *Pwd;                  // Password connect info
//int   Cto;                  // Connect timeout
//int   Qto;                  // Query timeout
	int   Fsize;								// Fetch size
	bool  Scrollable;						// Scrollable cursor
} JDBCPARM, *PJPARM;

/***********************************************************************/
/*  JDBC catalog function prototypes.                                  */
/***********************************************************************/
#if defined(PROMPT_OK)
char   *JDBCCheckConnection(PGLOBAL g, char *dsn, int cop);
#endif   // PROMPT_OK
//PQRYRES JDBCDataSources(PGLOBAL g, int maxres, bool info);
PQRYRES JDBCColumns(PGLOBAL g, char *jpath, char *db, char *table,
	char *colpat, int maxres, bool info, PJPARM sop);
PQRYRES JDBCSrcCols(PGLOBAL g, char *jpath, char *src, PJPARM sop);
PQRYRES JDBCTables(PGLOBAL g, char *jpath, char *db, char *tabpat,
	char *tabtyp, int maxres, bool info, PJPARM sop);
PQRYRES JDBCDrivers(PGLOBAL g, char *jpath, int maxres, bool info);
