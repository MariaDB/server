/************ Jdbconn C++ Functions Source Code File (.CPP) ************/
/*  Name: JDBCONN.CPP  Version 1.0                                     */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2016         */
/*                                                                     */
/*  This file contains the JDBC connection classes functions.          */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include <my_global.h>
#include <m_string.h>
#if defined(__WIN__)
//nclude <io.h>
//nclude <fcntl.h>
#include <direct.h>                      // for getcwd
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif
//#include <windows.h>
#else
#if defined(UNIX)
#include <errno.h>
#else
//nclude <io.h>
#endif
//nclude <fcntl.h>
#define NODW
#endif

/***********************************************************************/
/*  Required objects includes.                                         */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xobject.h"
#include "xtable.h"
#include "jdbccat.h"
#include "tabjdbc.h"
//#include "jdbconn.h"
//#include "plgcnx.h"                       // For DB types
#include "resource.h"
#include "valblk.h"
#include "osutil.h"


#if defined(__WIN__)
extern "C" HINSTANCE s_hModule;           // Saved module handle
#endif // __WIN__

int GetConvSize();

/***********************************************************************/
/*  Some macro's (should be defined elsewhere to be more accessible)   */
/***********************************************************************/
#if defined(_DEBUG)
#define ASSERT(f)          assert(f)
#define DEBUG_ONLY(f)      (f)
#else   // !_DEBUG
#define ASSERT(f)          ((void)0)
#define DEBUG_ONLY(f)      ((void)0)
#endif  // !_DEBUG

/***********************************************************************/
/*  GetJDBCType: returns the SQL_TYPE corresponding to a PLG type.      */
/***********************************************************************/
static short GetJDBCType(int type)
{
	short tp = 0;																  // NULL

	switch (type) {
	case TYPE_STRING:    tp = 12; break;					// VARCHAR
	case TYPE_SHORT:     tp = 5;  break;					// SMALLINT
	case TYPE_INT:       tp = 4;  break;					// INTEGER
	case TYPE_DATE:      tp = 93; break;					// DATE
//case TYPE_TIME:      tp = 92; break;					// TIME
//case TYPE_TIMESTAMP: tp = 93; break;					// TIMESTAMP
	case TYPE_BIGINT:    tp = -5; break;          // BIGINT
	case TYPE_DOUBLE:    tp = 8;  break;					// DOUBLE
	case TYPE_TINY:      tp = -6; break;					// TINYINT
	case TYPE_DECIM:     tp = 3;  break;					// DECIMAL
	} // endswitch type

	return tp;
} // end of GetJDBCType

/***********************************************************************/
/*  TranslateJDBCType: translate a JDBC Type to a PLG type.            */
/***********************************************************************/
int TranslateJDBCType(int stp, int prec, int& len, char& v)
{
	int type;

	switch (stp) {
	case -1:  // LONGVARCHAR
		len = MY_MIN(abs(len), GetConvSize());
	case 12:  // VARCHAR
		v = 'V';
	case 1:   // CHAR
		type = TYPE_STRING;
		break;
	case 2:   // NUMERIC
	case 3:   // DECIMAL
		type = TYPE_DECIM;
		break;
	case 4:   // INTEGER
		type = TYPE_INT;
		break;
	case 5:   // SMALLINT
		type = TYPE_SHORT;
		break;
	case -6:  // TINYINT
	case -7:  // BIT
		type = TYPE_TINY;
		break;
	case 6:   // FLOAT
	case 7:   // REAL
	case 8:   // DOUBLE
		type = TYPE_DOUBLE;
		break;
	case 93:  // TIMESTAMP
		type = TYPE_DATE;
		len = 19 + ((prec) ? (prec+1) : 0);
		v = 'S';
		break;
	case 91:  // TYPE_DATE
		type = TYPE_DATE;
		len = 10;
		v = 'D';
		break;
	case 92:  // TYPE_TIME
		type = TYPE_DATE;
		len = 8 + ((prec) ? (prec+1) : 0);
		v = 'T';
		break;
	case -5:  // BIGINT
		type = TYPE_BIGINT;
		break;
	case 0:   // NULL
	case -2:  // BINARY
	case -3:  // VARBINARY
	case -4:  // LONGVARBINARY
	default:
		type = TYPE_ERROR;
		len = 0;
	} // endswitch type

	return type;
} // end of TranslateJDBCType

/***********************************************************************/
/*  Allocate the structure used to refer to the result set.            */
/***********************************************************************/
static JCATPARM *AllocCatInfo(PGLOBAL g, JCATINFO fid, char *db,
	char *tab, PQRYRES qrp)
{
	size_t    m, n;
	JCATPARM *cap;

#if defined(_DEBUG)
	assert(qrp);
#endif

	// Save stack and allocation environment and prepare error return
	if (g->jump_level == MAX_JUMP) {
		strcpy(g->Message, MSG(TOO_MANY_JUMPS));
		return NULL;
	} // endif jump_level

	if (setjmp(g->jumper[++g->jump_level]) != 0) {
		printf("%s\n", g->Message);
		cap = NULL;
		goto fin;
	} // endif rc

	m = (size_t)qrp->Maxres;
	n = (size_t)qrp->Nbcol;
	cap = (JCATPARM *)PlugSubAlloc(g, NULL, sizeof(JCATPARM));
	memset(cap, 0, sizeof(JCATPARM));
	cap->Id = fid;
	cap->Qrp = qrp;
	cap->DB = (PUCHAR)db;
	cap->Tab = (PUCHAR)tab;
//cap->Vlen = (SQLLEN* *)PlugSubAlloc(g, NULL, n * sizeof(SQLLEN *));

//for (i = 0; i < n; i++)
//	cap->Vlen[i] = (SQLLEN *)PlugSubAlloc(g, NULL, m * sizeof(SQLLEN));

//cap->Status = (UWORD *)PlugSubAlloc(g, NULL, m * sizeof(UWORD));

fin:
	g->jump_level--;
	return cap;
} // end of AllocCatInfo

/***********************************************************************/
/*  JDBCColumns: constructs the result blocks containing all columns   */
/*  of a JDBC table that will be retrieved by GetData commands.        */
/***********************************************************************/
PQRYRES JDBCColumns(PGLOBAL g, char *jpath, char *db, char *table,
	char *colpat, int maxres, bool info, PJPARM sjp)
{
	int  buftyp[] = {TYPE_STRING, TYPE_STRING, TYPE_STRING, TYPE_STRING,
									 TYPE_SHORT,  TYPE_STRING, TYPE_INT,    TYPE_INT,
									 TYPE_SHORT,  TYPE_SHORT,  TYPE_SHORT,  TYPE_STRING};
	XFLD fldtyp[] = {FLD_CAT,   FLD_SCHEM,    FLD_TABNAME, FLD_NAME,
								   FLD_TYPE,  FLD_TYPENAME, FLD_PREC,    FLD_LENGTH,
								   FLD_SCALE, FLD_RADIX,    FLD_NULL,    FLD_REM};
	unsigned int length[] = {0, 0, 0, 0, 6, 0, 10, 10, 6, 6, 6, 0};
	bool     b[] = {true, true, false, false, false, false, false, false, true, true, false, true};
	int      i, n, ncol = 12;
	PCOLRES  crp;
	PQRYRES  qrp;
	JCATPARM *cap;
	JDBConn *jcp = NULL;

	/************************************************************************/
	/*  Do an evaluation of the result size.                                */
	/************************************************************************/
	if (!info) {
		jcp = new(g)JDBConn(g, NULL);

		if (jcp->Open(jpath, sjp) != RC_OK)  // openReadOnly + noJDBCdialog
			return NULL;

		if (table && !strchr(table, '%')) {
			// We fix a MySQL limit because some data sources return 32767
			n = jcp->GetMaxValue(1);  // MAX_COLUMNS_IN_TABLE)
			maxres = (n > 0) ? MY_MIN(n, 4096) : 4096;
		} else if (!maxres)
			maxres = 20000;

		//  n = jcp->GetMaxValue(2);   MAX_CATALOG_NAME_LEN
		//  length[0] = (n) ? (n + 1) : 0;
		//  n = jcp->GetMaxValue(3);   MAX_SCHEMA_NAME_LEN
		//  length[1] = (n) ? (n + 1) : 0;
		//  n = jcp->GetMaxValue(4);   MAX_TABLE_NAME_LEN
		//  length[2] = (n) ? (n + 1) : 0;
		n = jcp->GetMaxValue(5);    // MAX_COLUMN_NAME_LEN
		length[3] = (n > 0) ? (n + 1) : 128;
	} else {                 // Info table
		maxres = 0;
		length[0] = 128;
		length[1] = 128;
		length[2] = 128;
		length[3] = 128;
		length[5] = 30;
		length[11] = 255;
	} // endif jcp

	if (trace)
		htrc("JDBCColumns: max=%d len=%d,%d,%d,%d\n",
		maxres, length[0], length[1], length[2], length[3]);

	/************************************************************************/
	/*  Allocate the structures used to refer to the result set.            */
	/************************************************************************/
	qrp = PlgAllocResult(g, ncol, maxres, IDS_COLUMNS,
		buftyp, fldtyp, length, false, true);

	for (i = 0, crp = qrp->Colresp; crp; i++, crp = crp->Next)
		if (b[i])
			crp->Kdata->SetNullable(true);

	if (info || !qrp)                      // Info table
		return qrp;

	if (trace)
		htrc("Getting col results ncol=%d\n", qrp->Nbcol);

	if (!(cap = AllocCatInfo(g, CAT_COL, db, table, qrp)))
		return NULL;

	cap->Pat = (PUCHAR)colpat;

	/************************************************************************/
	/*  Now get the results into blocks.                                    */
	/************************************************************************/
	if ((n = jcp->GetCatInfo(cap)) >= 0) {
		qrp->Nblin = n;
		//  ResetNullValues(cap);

		if (trace)
			htrc("Columns: NBCOL=%d NBLIN=%d\n", qrp->Nbcol, qrp->Nblin);

	} else
		qrp = NULL;

	/* Cleanup */
	jcp->Close();

	/************************************************************************/
	/*  Return the result pointer for use by GetData routines.              */
	/************************************************************************/
	return qrp;
} // end of JDBCColumns

/**************************************************************************/
/*  JDBCSrcCols: constructs the result blocks containing the              */
/*  description of all the columns of a Srcdef option.                    */
/**************************************************************************/
PQRYRES JDBCSrcCols(PGLOBAL g, char *jpath, char *src, PJPARM sjp)
{
	JDBConn *jcp = new(g)JDBConn(g, NULL);

	if (jcp->Open(jpath, sjp))
		return NULL;

	return jcp->GetMetaData(g, src);
} // end of JDBCSrcCols

/**************************************************************************/
/*  JDBCTables: constructs the result blocks containing all tables in     */
/*  an JDBC database that will be retrieved by GetData commands.          */
/**************************************************************************/
PQRYRES JDBCTables(PGLOBAL g, char *jpath, char *db, char *tabpat,
	                 char *tabtyp, int maxres, bool info, PJPARM sjp)
{
	int      buftyp[] = {TYPE_STRING, TYPE_STRING, TYPE_STRING,
		                   TYPE_STRING, TYPE_STRING};
	XFLD     fldtyp[] = {FLD_CAT, FLD_SCHEM, FLD_NAME, FLD_TYPE, FLD_REM};
	unsigned int length[] = {0, 0, 0, 16, 0};
	bool     b[] = {true, true, false, false, true};
	int      i, n, ncol = 5;
	PCOLRES  crp;
	PQRYRES  qrp;
	JCATPARM *cap;
	JDBConn *jcp = NULL;

	/************************************************************************/
	/*  Do an evaluation of the result size.                                */
	/************************************************************************/
	if (!info) {
		/**********************************************************************/
		/*  Open the connection with the JDBC data source.                    */
		/**********************************************************************/
		jcp = new(g)JDBConn(g, NULL);

		if (jcp->Open(jpath, sjp) == RC_FX)
			return NULL;

		if (!maxres)
			maxres = 10000;                 // This is completely arbitrary

		n = jcp->GetMaxValue(2);					// Max catalog name length

		if (n < 0)
			return NULL;

		length[0] = (n) ? (n + 1) : 0;
		n = jcp->GetMaxValue(3);					// Max schema name length
		length[1] = (n) ? (n + 1) : 0;
		n = jcp->GetMaxValue(4);					// Max table name length
		length[2] = (n) ? (n + 1) : 128;
	} else {
		maxres = 0;
		length[0] = 128;
		length[1] = 128;
		length[2] = 128;
		length[4] = 255;
	} // endif info

	if (trace)
		htrc("JDBCTables: max=%d len=%d,%d\n", maxres, length[0], length[1]);

	/************************************************************************/
	/*  Allocate the structures used to refer to the result set.            */
	/************************************************************************/
	qrp = PlgAllocResult(g, ncol, maxres, IDS_TABLES, buftyp,
		fldtyp, length, false, true);

	for (i = 0, crp = qrp->Colresp; crp; i++, crp = crp->Next)
		if (b[i])
			crp->Kdata->SetNullable(true);

	if (info || !qrp)
		return qrp;

	if (!(cap = AllocCatInfo(g, CAT_TAB, db, tabpat, qrp)))
		return NULL;

	cap->Pat = (PUCHAR)tabtyp;

	if (trace)
		htrc("Getting table results ncol=%d\n", cap->Qrp->Nbcol);

	/************************************************************************/
	/*  Now get the results into blocks.                                    */
	/************************************************************************/
	if ((n = jcp->GetCatInfo(cap)) >= 0) {
		qrp->Nblin = n;
		//  ResetNullValues(cap);

		if (trace)
			htrc("Tables: NBCOL=%d NBLIN=%d\n", qrp->Nbcol, qrp->Nblin);

	} else
		qrp = NULL;

	/************************************************************************/
	/*  Close any local connection.                                         */
	/************************************************************************/
	jcp->Close();

	/************************************************************************/
	/*  Return the result pointer for use by GetData routines.              */
	/************************************************************************/
	return qrp;
} // end of JDBCTables

/*************************************************************************/
/*  JDBCDrivers: constructs the result blocks containing all JDBC        */
/*  drivers available on the local host.                                 */
/*  Called with info=true to have result column names.                   */
/*************************************************************************/
PQRYRES JDBCDrivers(PGLOBAL g, char *jpath, int maxres, bool info)
{
	int      buftyp[] ={TYPE_STRING, TYPE_STRING, TYPE_STRING, TYPE_STRING};
	XFLD     fldtyp[] ={FLD_NAME, FLD_EXTRA, FLD_DEFAULT, FLD_REM };
	unsigned int length[] ={ 128, 32, 4, 256 };
	bool     b[] ={ false, false, false, true };
	int      i, ncol = 4;
	PCOLRES  crp;
	PQRYRES  qrp;
	JDBConn *jcp = NULL;

	/************************************************************************/
	/*  Do an evaluation of the result size.                                */
	/************************************************************************/
	if (!info) {
		jcp = new(g) JDBConn(g, NULL);

		if (jcp->Open(jpath, NULL) != RC_OK)
			return NULL;

		if (!maxres)
			maxres = 256;         // Estimated max number of drivers

	} else
		maxres = 0;

	if (trace)
		htrc("JDBCDrivers: max=%d len=%d\n", maxres, length[0]);

	/************************************************************************/
	/*  Allocate the structures used to refer to the result set.            */
	/************************************************************************/
	qrp = PlgAllocResult(g, ncol, maxres, 0, buftyp, fldtyp, length, false, true);

	for (i = 0, crp = qrp->Colresp; crp; i++, crp = crp->Next) {
		if (b[i])
			crp->Kdata->SetNullable(true);

		switch (i) {
		case 0: crp->Name = "Name";        break;
		case 1: crp->Name = "Version";     break;
		case 2: crp->Name = "Compliant";   break;
		case 3: crp->Name = "Description"; break;
		}	// endswitch

	} // endfor i

	/************************************************************************/
	/*  Now get the results into blocks.                                    */
	/************************************************************************/
	if (!info && qrp && jcp->GetDrivers(qrp))
		qrp = NULL;

	if (!info)
		jcp->Close();

	/************************************************************************/
	/*  Return the result pointer for use by GetData routines.              */
	/************************************************************************/
	return qrp;
} // end of JDBCDrivers

#if 0
/*************************************************************************/
/*  JDBCDataSources: constructs the result blocks containing all JDBC    */
/*  data sources available on the local host.                            */
/*  Called with info=true to have result column names.                   */
/*************************************************************************/
PQRYRES JDBCDataSources(PGLOBAL g, int maxres, bool info)
{
	int      buftyp[] ={ TYPE_STRING, TYPE_STRING };
	XFLD     fldtyp[] ={ FLD_NAME, FLD_REM };
	unsigned int length[] ={ 0, 256 };
	bool     b[] ={ false, true };
	int      i, n = 0, ncol = 2;
	PCOLRES  crp;
	PQRYRES  qrp;
	JDBConn *jcp = NULL;

	/************************************************************************/
	/*  Do an evaluation of the result size.                                */
	/************************************************************************/
	if (!info) {
		jcp = new(g)JDBConn(g, NULL);
		n = jcp->GetMaxValue(SQL_MAX_DSN_LENGTH);
		length[0] = (n) ? (n + 1) : 256;

		if (!maxres)
			maxres = 512;         // Estimated max number of data sources

	} else {
		length[0] = 256;
		maxres = 0;
	} // endif info

	if (trace)
		htrc("JDBCDataSources: max=%d len=%d\n", maxres, length[0]);

	/************************************************************************/
	/*  Allocate the structures used to refer to the result set.            */
	/************************************************************************/
	qrp = PlgAllocResult(g, ncol, maxres, IDS_DSRC,
		buftyp, fldtyp, length, false, true);

	for (i = 0, crp = qrp->Colresp; crp; i++, crp = crp->Next)
		if (b[i])
			crp->Kdata->SetNullable(true);

	/************************************************************************/
	/*  Now get the results into blocks.                                    */
	/************************************************************************/
	if (!info && qrp && jcp->GetDataSources(qrp))
		qrp = NULL;

	/************************************************************************/
	/*  Return the result pointer for use by GetData routines.              */
	/************************************************************************/
	return qrp;
} // end of JDBCDataSources

/**************************************************************************/
/*  PrimaryKeys: constructs the result blocks containing all the          */
/*  JDBC catalog information concerning primary keys.                     */
/**************************************************************************/
PQRYRES JDBCPrimaryKeys(PGLOBAL g, JDBConn *op, char *dsn, char *table)
{
	static int buftyp[] ={ TYPE_STRING, TYPE_STRING, TYPE_STRING,
		TYPE_STRING, TYPE_SHORT, TYPE_STRING };
	static unsigned int length[] ={ 0, 0, 0, 0, 6, 128 };
	int      n, ncol = 5;
	int     maxres;
	PQRYRES  qrp;
	JCATPARM *cap;
	JDBConn *jcp = op;

	if (!op) {
		/**********************************************************************/
		/*  Open the connection with the JDBC data source.                    */
		/**********************************************************************/
		jcp = new(g)JDBConn(g, NULL);

		if (jcp->Open(dsn, 2) < 1)        // 2 is openReadOnly
			return NULL;

	} // endif op

	/************************************************************************/
	/*  Do an evaluation of the result size.                                */
	/************************************************************************/
	n = jcp->GetMaxValue(SQL_MAX_COLUMNS_IN_TABLE);
	maxres = (n) ? (int)n : 250;
	n = jcp->GetMaxValue(SQL_MAX_CATALOG_NAME_LEN);
	length[0] = (n) ? (n + 1) : 128;
	n = jcp->GetMaxValue(SQL_MAX_SCHEMA_NAME_LEN);
	length[1] = (n) ? (n + 1) : 128;
	n = jcp->GetMaxValue(SQL_MAX_TABLE_NAME_LEN);
	length[2] = (n) ? (n + 1) : 128;
	n = jcp->GetMaxValue(SQL_MAX_COLUMN_NAME_LEN);
	length[3] = (n) ? (n + 1) : 128;

	if (trace)
		htrc("JDBCPrimaryKeys: max=%d len=%d,%d,%d\n",
		maxres, length[0], length[1], length[2]);

	/************************************************************************/
	/*  Allocate the structure used to refer to the result set.             */
	/************************************************************************/
	qrp = PlgAllocResult(g, ncol, maxres, IDS_PKEY,
		buftyp, NULL, length, false, true);

	if (trace)
		htrc("Getting pkey results ncol=%d\n", qrp->Nbcol);

	cap = AllocCatInfo(g, CAT_KEY, NULL, table, qrp);

	/************************************************************************/
	/*  Now get the results into blocks.                                    */
	/************************************************************************/
	if ((n = jcp->GetCatInfo(cap)) >= 0) {
		qrp->Nblin = n;
		//  ResetNullValues(cap);

		if (trace)
			htrc("PrimaryKeys: NBCOL=%d NBLIN=%d\n", qrp->Nbcol, qrp->Nblin);

	} else
		qrp = NULL;

	/************************************************************************/
	/*  Close any local connection.                                         */
	/************************************************************************/
	if (!op)
		jcp->Close();

	/************************************************************************/
	/*  Return the result pointer for use by GetData routines.              */
	/************************************************************************/
	return qrp;
} // end of JDBCPrimaryKeys

/**************************************************************************/
/*  Statistics: constructs the result blocks containing statistics        */
/*  about one or several tables to be retrieved by GetData commands.      */
/**************************************************************************/
PQRYRES JDBCStatistics(PGLOBAL g, JDBConn *op, char *dsn, char *pat,
	int un, int acc)
{
	static int buftyp[] ={ TYPE_STRING,
		TYPE_STRING, TYPE_STRING, TYPE_SHORT, TYPE_STRING,
		TYPE_STRING, TYPE_SHORT, TYPE_SHORT, TYPE_STRING,
		TYPE_STRING, TYPE_INT, TYPE_INT, TYPE_STRING };
	static unsigned int length[] ={ 0, 0, 0, 6, 0, 0, 6, 6, 0, 2, 10, 10, 128 };
	int      n, ncol = 13;
	int     maxres;
	PQRYRES  qrp;
	JCATPARM *cap;
	JDBConn *jcp = op;

	if (!op) {
		/**********************************************************************/
		/*  Open the connection with the JDBC data source.                    */
		/**********************************************************************/
		jcp = new(g)JDBConn(g, NULL);

		if (jcp->Open(dsn, 2) < 1)        // 2 is openReadOnly
			return NULL;

	} // endif op

	/************************************************************************/
	/*  Do an evaluation of the result size.                                */
	/************************************************************************/
	n = 1 + jcp->GetMaxValue(SQL_MAX_COLUMNS_IN_INDEX);
	maxres = (n) ? (int)n : 32;
	n = jcp->GetMaxValue(SQL_MAX_SCHEMA_NAME_LEN);
	length[1] = (n) ? (n + 1) : 128;
	n = jcp->GetMaxValue(SQL_MAX_TABLE_NAME_LEN);
	length[2] = length[5] = (n) ? (n + 1) : 128;
	n = jcp->GetMaxValue(SQL_MAX_CATALOG_NAME_LEN);
	length[0] = length[4] = (n) ? (n + 1) : length[2];
	n = jcp->GetMaxValue(SQL_MAX_COLUMN_NAME_LEN);
	length[7] = (n) ? (n + 1) : 128;

	if (trace)
		htrc("SemStatistics: max=%d pat=%s\n", maxres, SVP(pat));

	/************************************************************************/
	/*  Allocate the structure used to refer to the result set.             */
	/************************************************************************/
	qrp = PlgAllocResult(g, ncol, maxres, IDS_STAT,
		buftyp, NULL, length, false, true);

	if (trace)
		htrc("Getting stat results ncol=%d\n", qrp->Nbcol);

	cap = AllocCatInfo(g, CAT_STAT, NULL, pat, qrp);
	cap->Unique = (un < 0) ? SQL_INDEX_UNIQUE : (UWORD)un;
	cap->Accuracy = (acc < 0) ? SQL_QUICK : (UWORD)acc;

	/************************************************************************/
	/*  Now get the results into blocks.                                    */
	/************************************************************************/
	if ((n = jcp->GetCatInfo(cap)) >= 0) {
		qrp->Nblin = n;
		//  ResetNullValues(cap);

		if (trace)
			htrc("Statistics: NBCOL=%d NBLIN=%d\n", qrp->Nbcol, qrp->Nblin);

	} else
		qrp = NULL;

	/************************************************************************/
	/*  Close any local connection.                                         */
	/************************************************************************/
	if (!op)
		jcp->Close();

	/************************************************************************/
	/*  Return the result pointer for use by GetData routines.              */
	/************************************************************************/
	return qrp;
} // end of Statistics
#endif // 0

/***********************************************************************/
/*  JDBConn construction/destruction.                                  */
/***********************************************************************/
JDBConn::JDBConn(PGLOBAL g, TDBJDBC *tdbp)
{
	m_G = g;
	m_Tdb = tdbp;
	jvm = nullptr;            // Pointer to the JVM (Java Virtual Machine)
	env= nullptr;             // Pointer to native interface
	jdi = nullptr;						// Pointer to the JdbcInterface class
	job = nullptr;						// The JdbcInterface class object
	xqid = xuid = xid = grs = readid = fetchid = typid = nullptr;
	prepid = xpid = pcid = nullptr;
//m_LoginTimeout = DEFAULT_LOGIN_TIMEOUT;
//m_QueryTimeout = DEFAULT_QUERY_TIMEOUT;
//m_UpdateOptions = 0;
	m_Driver = NULL;
	m_Url = NULL;
	m_User = NULL;
	m_Pwd = NULL;
	m_Ncol = 0;
	m_Aff = 0;
	m_Rows = 0;
	m_Fetch = 0;
	m_RowsetSize = 0;
	m_Updatable = true;
	m_Transact = false;
	m_Scrollable = false;
	m_Full = false;
	m_Opened = false;
	m_IDQuoteChar[0] = '"';
	m_IDQuoteChar[1] = 0;
	//*m_ErrMsg = '\0';
} // end of JDBConn

//JDBConn::~JDBConn()
//  {
//if (Connected())
//  EndCom();

//  } // end of ~JDBConn

#if 0
/***********************************************************************/
/*  Screen for errors.                                                 */
/***********************************************************************/
bool JDBConn::Check(RETCODE rc)
{
	switch (rc) {
	case SQL_SUCCESS_WITH_INFO:
		if (trace) {
			DJX x(rc);

			if (x.BuildErrorMessage(this, m_hstmt))
				htrc("JDBC Success With Info, hstmt=%p %s\n",
				m_hstmt, x.GetErrorMessage(0));

		} // endif trace

		// Fall through
	case SQL_SUCCESS:
	case SQL_NO_DATA_FOUND:
		return true;
	} // endswitch rc

	return false;
} // end of Check

/***********************************************************************/
/*  Utility routine.                                                   */
/***********************************************************************/
PSZ JDBConn::GetStringInfo(ushort infotype)
{
	//ASSERT(m_hdbc != SQL_NULL_HDBC);
	char   *p, buffer[MAX_STRING_INFO];
	SWORD   result;
	RETCODE rc;

	rc = SQLGetInfo(m_hdbc, infotype, buffer, sizeof(buffer), &result);

	if (!Check(rc)) {
		ThrowDJX(rc, "SQLGetInfo");  // Temporary
		//  *buffer = '\0';
	} // endif rc

	p = PlugDup(m_G, buffer);
	return p;
} // end of GetStringInfo

/***********************************************************************/
/*  Utility routines.                                                  */
/***********************************************************************/
void JDBConn::OnSetOptions(HSTMT hstmt)
{
	RETCODE rc;
	ASSERT(m_hdbc != SQL_NULL_HDBC);

	if ((signed)m_QueryTimeout != -1) {
		// Attempt to set query timeout.  Ignore failure
		rc = SQLSetStmtOption(hstmt, SQL_QUERY_TIMEOUT, m_QueryTimeout);

		if (!Check(rc))
			// don't attempt it again
			m_QueryTimeout = (DWORD)-1;

	} // endif m_QueryTimeout

	if (m_RowsetSize > 0) {
		// Attempt to set rowset size.
		// In case of failure reset it to 0 to use Fetch.
		rc = SQLSetStmtOption(hstmt, SQL_ROWSET_SIZE, m_RowsetSize);

		if (!Check(rc))
			// don't attempt it again
			m_RowsetSize = 0;

	} // endif m_RowsetSize

} // end of OnSetOptions
#endif // 0

/***********************************************************************/
/*  Utility routine.                                                   */
/***********************************************************************/
int JDBConn::GetMaxValue(int n)
{
	jmethodID maxid = env->GetMethodID(jdi, "GetMaxValue", "(I)I");

	if (maxid == nullptr) {
		strcpy(m_G->Message, "ERROR: method GetMaxValue not found !");
		return -1;
	} // endif maxid

	// call method
	return (int)env->CallIntMethod(job, maxid, n);
} // end of GetMaxValue

/***********************************************************************/
/*  Open: connect to a data source.                                    */
/***********************************************************************/
int JDBConn::Open(PSZ jpath, PJPARM sop)
{
	PGLOBAL& g = m_G;
	PSTRG    jpop = new(g) STRING(g, 512, "-Djava.class.path=");
	char     sep;

#if defined(__WIN__)
	sep = ';';
#else
	sep = ':';
#endif

	if (sop) {
		m_Driver = sop->Driver;
		m_Url = sop->Url;
		m_User = sop->User;
		m_Pwd = sop->Pwd;
		m_Scrollable = sop->Scrollable;
		m_RowsetSize = sop->Fsize;
		//m_LoginTimeout = sop->Cto;
		//m_QueryTimeout = sop->Qto;
		//m_UseCnc = sop->UseCnc;
	} // endif sop

	//================== prepare loading of Java VM ============================
	JavaVMInitArgs vm_args;                        // Initialization arguments
	JavaVMOption* options = new JavaVMOption[1];   // JVM invocation options

	// where to find java .class
	jpop->Append(getenv("CLASSPATH"));

	if (jpath) {
		jpop->Append(sep);
		jpop->Append(jpath);
	}	// endif jpath

	options[0].optionString =	jpop->GetStr();
//options[1].optionString =	"-verbose:jni";
	vm_args.version = JNI_VERSION_1_6;             // minimum Java version
	vm_args.nOptions = 1;                          // number of options
	vm_args.options = options;
	vm_args.ignoreUnrecognized = false; // invalid options make the JVM init fail

	//=============== load and initialize Java VM and JNI interface =============
	jint rc = JNI_CreateJavaVM(&jvm, (void**)&env, &vm_args);  // YES !!
	delete options;    // we then no longer need the initialisation options.

	switch (rc) {
	case JNI_OK:
		strcpy(g->Message, "VM successfully created");
		break;
	case JNI_ERR:
		strcpy(g->Message, "Initialising JVM failed: unknown error");
		return RC_FX;
	case JNI_EDETACHED:
		strcpy(g->Message, "Thread detached from the VM");
		return RC_FX;
	case JNI_EVERSION:
		strcpy(g->Message, "JNI version error");
		return RC_FX;
	case JNI_ENOMEM:
		strcpy(g->Message, "Not enough memory");
		return RC_FX;
	case JNI_EEXIST:
		strcpy(g->Message, "VM already created");
		{
			JavaVM* jvms[1];
			jsize   jsz;

			rc = JNI_GetCreatedJavaVMs(jvms, 1, &jsz);

			if (rc == JNI_OK && jsz == 1) {
				JavaVMAttachArgs args;

				args.version = JNI_VERSION_1_6;
				args.name = NULL;
				args.group = NULL;
				jvm = jvms[0];
				rc = jvm->AttachCurrentThread((void**)&env, &args);
			} // endif rc
		}	// end of block

		if (rc == JNI_OK)
			break;
		else
			return RC_FX;

	case JNI_EINVAL:
		strcpy(g->Message, "Invalid arguments");
		return RC_FX;
	default:
		sprintf(g->Message, "Unknown return code %d", rc);
		return RC_FX;
	} // endswitch rc

	//=============== Display JVM version =======================================
//jint ver = env->GetVersion();
//cout << ((ver>>16)&0x0f) << "."<<(ver&0x0f) << endl;

	// try to find the JdbcInterface class
	jdi = env->FindClass("JdbcInterface");

	if (jdi == nullptr) {
		strcpy(g->Message, "ERROR: class JdbcInterface not found !");
		return RC_FX;
	} // endif jdi

	// if class found, continue
	jmethodID ctor = env->GetMethodID(jdi, "<init>", "()V");

	if (ctor == nullptr) {
		strcpy(g->Message, "ERROR: JdbcInterface constructor not found !");
		return RC_FX;
	} else
		job = env->NewObject(jdi, ctor);

	// If the object is successfully constructed, 
	// we can then search for the method we want to call, 
	// and invoke it for the object:
	if (job == nullptr) {
		strcpy(g->Message, "JdbcInterface class object not constructed !");
		return RC_FX;
	} // endif job

	if (!sop)						 // DRIVER catalog table
		return RC_OK;

	jmethodID cid = env->GetMethodID(jdi, "JdbcConnect", "([Ljava/lang/String;IZ)I");

	if (env->ExceptionCheck()) {
		strcpy(g->Message, "ERROR: method JdbcConnect() not found!");
		env->ExceptionDescribe();
		env->ExceptionClear();
		return RC_FX;
	} // endif Check

	// Build the java string array
	jobjectArray parms = env->NewObjectArray(4,    // constructs java array of 4
		env->FindClass("java/lang/String"), NULL);   // Strings

	// change some elements
	if (m_Driver)
		env->SetObjectArrayElement(parms, 0, env->NewStringUTF(m_Driver));

	if (m_Url)
		env->SetObjectArrayElement(parms, 1, env->NewStringUTF(m_Url));

	if (m_User)
		env->SetObjectArrayElement(parms, 2, env->NewStringUTF(m_User));

	if (m_Pwd)
		env->SetObjectArrayElement(parms, 3, env->NewStringUTF(m_Pwd));

	// call method
	rc = env->CallIntMethod(job, cid, parms, m_RowsetSize, m_Scrollable);

	// Not used anymore
	env->DeleteLocalRef(parms);

	if (rc != (jint)0) {
		strcpy(g->Message, "Connection failed");
		return RC_FX;
	} // endif rc

	typid = env->GetMethodID(jdi, "ColumnType", "(ILjava/lang/String;)I");

	if (env->ExceptionCheck()) {
		strcpy(g->Message, "ERROR: method ColumnType() not found!");
		env->ExceptionDescribe();
		env->ExceptionClear();
		return RC_FX;
	} else
		m_Opened = true;

	return RC_OK;
} // end of Open

/***********************************************************************/
/*  Execute an SQL command.                                            */
/***********************************************************************/
int JDBConn::ExecSQLcommand(char *sql)
{
	int      rc = RC_NF;
	jint     n;
	jstring  qry;
	PGLOBAL& g = m_G;

	if (xid == nullptr) {
		// Get the methods used to execute a query and get the result
		xid = env->GetMethodID(jdi, "Execute", "(Ljava/lang/String;)I");

		if (xid == nullptr) {
			strcpy(g->Message, "Cannot find method Execute");
			return RC_FX;
		} else
			grs = env->GetMethodID(jdi, "GetResult", "()I");

		if (grs == nullptr) {
			strcpy(g->Message, "Cannot find method GetResult");
			return RC_FX;
		} // endif grs

	}	// endif xid

	qry = env->NewStringUTF(sql);
	n = env->CallIntMethod(job, xid, qry);
	env->DeleteLocalRef(qry);

	if (n < 0) {
		sprintf(g->Message, "Error executing %s", sql);
		return RC_FX;
	} // endif n

	if ((m_Ncol = env->CallIntMethod(job, grs))) {
		strcpy(g->Message, "Result set column number");
		rc = RC_OK;						// A result set was returned
	} else {
		m_Aff = (int)n;			  // Affected rows
		strcpy(g->Message, "Affected rows");
	} // endif ncol

	return rc;
} // end of ExecSQLcommand

/***********************************************************************/
/*  Fetch next row.                                                    */
/***********************************************************************/
int JDBConn::Fetch(int pos)
{
	jint rc;

	if (m_Full)						// Result set has one row
		return 1;

	if (pos) {
		if (!m_Scrollable) {
			strcpy(m_G->Message, "Cannot fetch(pos) if FORWARD ONLY");
		  return -1;
	  }	else if (fetchid == nullptr) {
			fetchid = env->GetMethodID(jdi, "Fetch", "(I)Z");

			if (fetchid == nullptr) {
				strcpy(m_G->Message, "Cannot find method Fetch");
				return -1;
			}	// endif fetchid

		}	// endif's

		if (env->CallBooleanMethod(job, fetchid, pos))
			rc = m_Rows;
		else
			rc = -1;

	} else {
		if (readid == nullptr) {
			readid = env->GetMethodID(jdi, "ReadNext", "()I");

			if (readid == nullptr) {
				strcpy(m_G->Message, "Cannot find method ReadNext");
				return -1;
			}	// endif readid

		}	// endif readid

		rc = env->CallBooleanMethod(job, readid);

		if (rc >= 0) {
			if (rc == 0)
				m_Full = (m_Fetch == 1);
			else
				m_Fetch++;

			m_Rows += (int)rc;
		} else
			strcpy(m_G->Message, "Error fetching next row");

	} // endif pos

	return (int)rc;
} // end of Fetch

/***********************************************************************/
/*  Restart from beginning of result set                               */
/***********************************************************************/
int JDBConn::Rewind(char *sql)
{
	int rbuf = -1;

	if (m_Full)
		rbuf = m_Rows;           // No need to "rewind"
	else if (m_Scrollable) {
		if (fetchid == nullptr) {
			fetchid = env->GetMethodID(jdi, "Fetch", "(I)Z");

			if (fetchid == nullptr) {
				strcpy(m_G->Message, "Cannot find method Fetch");
				return -1;
			}	// endif readid

		}	// endif readid

		jboolean b = env->CallBooleanMethod(job, fetchid, 0);

		rbuf = m_Rows;
	} else if (ExecSQLcommand(sql) != RC_FX)
		rbuf = 0;

	return rbuf;
} // end of Rewind

/***********************************************************************/
/*  Disconnect connection                                              */
/***********************************************************************/
void JDBConn::Close()
{
	if (m_Opened) {
		jint      rc;
		jmethodID did = env->GetMethodID(jdi, "JdbcDisconnect", "()I");

		if (did == nullptr)
			printf("ERROR: method JdbcDisconnect() not found !");
		else
			rc = env->CallIntMethod(job, did);

		rc = jvm->DetachCurrentThread();
		//rc = jvm->DestroyJavaVM();
		m_Opened = false;
	}	// endif m_Opened

} // end of Close

/***********************************************************************/
/*  Retrieve and set the column value from the result set.             */
/***********************************************************************/
void JDBConn::SetColumnValue(int rank, PSZ name, PVAL val)
{
	PGLOBAL&   g = m_G;
	jint       ctyp;
	jlong      dtv;
	jstring    cn, jn = nullptr;
	jobject    dob;
	jthrowable exc;
	jmethodID  fldid = nullptr;

	if (rank == 0)
		if (!name || (jn = env->NewStringUTF(name)) == nullptr) {
			sprintf(g->Message, "Fail to allocate jstring %s", SVP(name));
			longjmp(g->jumper[g->jump_level], TYPE_AM_JDBC);
		}	// endif name

	ctyp = env->CallIntMethod(job, typid, rank, jn);

	if ((exc = env->ExceptionOccurred()) != nullptr) {
		jboolean isCopy = false;
		jmethodID tid = env->GetMethodID(env->FindClass("java/lang/Object"), "toString", "()Ljava/lang/String;");
		jstring s = (jstring)env->CallObjectMethod(exc, tid);
		const char* utf = env->GetStringUTFChars(s, &isCopy);
		sprintf(g->Message, "SetColumnValue: %s", utf);
		env->DeleteLocalRef(s);
		env->ExceptionClear();
		longjmp(g->jumper[g->jump_level], TYPE_AM_JDBC);
	} // endif Check

	switch (ctyp) {
	case 12:          // VARCHAR
	case -1:          // LONGVARCHAR
	case 1:           // CHAR
		fldid = env->GetMethodID(jdi, "StringField",
			                       "(ILjava/lang/String;)Ljava/lang/String;");

		if (fldid != nullptr) {
			cn = (jstring)env->CallObjectMethod(job, fldid, (jint)rank, jn);

			if (cn) {
				const char *field = env->GetStringUTFChars(cn, (jboolean)false);
				val->SetValue_psz((PSZ)field);
			} else {
				val->Reset();
				val->SetNull(true);
			} // endif cn

		} else
			val->Reset();

		break;
	case 4:           // INTEGER
	case 5:           // SMALLINT
	case -6:          // TINYINT
		fldid = env->GetMethodID(jdi, "IntField", "(ILjava/lang/String;)I");

		if (fldid != nullptr)
			val->SetValue((int)env->CallIntMethod(job, fldid, rank, jn));
		else
			val->Reset();

		break;
	case 8:           // DOUBLE
	case 3:           // DECIMAL
		fldid = env->GetMethodID(jdi, "DoubleField", "(ILjava/lang/String;)D");

		if (fldid != nullptr)
			val->SetValue((double)env->CallDoubleMethod(job, fldid, rank, jn));
		else
			val->Reset();

		break;
	case 7:           // REAL
	case 6:           // FLOAT
		fldid = env->GetMethodID(jdi, "FloatField", "(ILjava/lang/String;)F");

		if (fldid != nullptr)
			val->SetValue((float)env->CallFloatMethod(job, fldid, rank, jn));
		else
			val->Reset();

		break;
	case 91:          // DATE
	case 92:          // TIME
	case 93:          // TIMESTAMP
		fldid = env->GetMethodID(jdi, "TimestampField",
			                       "(ILjava/lang/String;)Ljava/sql/Timestamp;");

		if (fldid != nullptr) {
			dob = env->CallObjectMethod(job, fldid, (jint)rank, jn);

			if (dob) {
				jclass jts = env->FindClass("java/sql/Timestamp");

				if (env->ExceptionCheck()) {
					val->Reset();
				} else {
					jmethodID getTime = env->GetMethodID(jts, "getTime", "()J");

					if (getTime != nullptr) {
						dtv = env->CallLongMethod(dob, getTime);
						val->SetValue((int)(dtv / 1000));
					} else
						val->Reset();

				} // endif check

			} else
				val->Reset();

		} else
			val->Reset();

		break;
	case -5:          // BIGINT
		fldid = env->GetMethodID(jdi, "BigintField", "(ILjava/lang/String;)J");

		if (fldid != nullptr)
			val->SetValue((long long)env->CallLongMethod(job, fldid, (jint)rank, jn));
		else
			val->Reset();

		break;
		/*			case java.sql.Types.SMALLINT:
		System.out.print(jdi.IntField(i));
		break;
		case java.sql.Types.BOOLEAN:
		System.out.print(jdi.BooleanField(i)); */
	default:
		val->Reset();
	} // endswitch Type

	if (rank == 0)
		env->DeleteLocalRef(jn);

} // end of SetColumnValue

/***********************************************************************/
/*  Prepare an SQL statement for insert.                               */
/***********************************************************************/
bool JDBConn::PrepareSQL(char *sql)
{
	if (prepid == nullptr) {
		prepid = env->GetMethodID(jdi, "CreatePrepStmt", "(Ljava/lang/String;)Z");

		if (prepid == nullptr) {
			strcpy(m_G->Message, "Cannot find method CreatePrepStmt");
			return true;
		}	// endif prepid

	} // endif prepid
	
	// Create the prepared statement
	jstring qry = env->NewStringUTF(sql);
	jboolean b = env->CallBooleanMethod(job, prepid, qry);
	env->DeleteLocalRef(qry);
	return (bool)b;
} // end of PrepareSQL

/***********************************************************************/
/*  Execute an SQL query that returns a result set.                    */
/***********************************************************************/
int JDBConn::ExecuteQuery(char *sql)
{
	jint     ncol;
	jstring  qry;
	PGLOBAL& g = m_G;

	if (xqid == nullptr) {
		// Get the methods used to execute a query and get the result
		xqid = env->GetMethodID(jdi, "ExecuteQuery", "(Ljava/lang/String;)I");

		if (xqid == nullptr) {
			strcpy(g->Message, "Cannot find method ExecuteQuery");
			return RC_FX;
		} // endif !xqid

	}	// endif xqid

	qry = env->NewStringUTF(sql);
	ncol = env->CallIntMethod(job, xqid, qry);
	env->DeleteLocalRef(qry);

	if (ncol < 0) {
		sprintf(g->Message, "Error executing %s: ncol = %d", sql, ncol);
		return RC_FX;
	} else {
		m_Ncol = (int)ncol;
		m_Aff = 0;			  // Affected rows
	} // endif ncol

	return RC_OK;
} // end of ExecuteQuery

/***********************************************************************/
/*  Execute an SQL query and get the affected rows.                    */
/***********************************************************************/
int JDBConn::ExecuteUpdate(char *sql)
{
	jint     n;
	jstring  qry;
	PGLOBAL& g = m_G;

	if (xuid == nullptr) {
		// Get the methods used to execute a query and get the affected rows
		xuid = env->GetMethodID(jdi, "ExecuteUpdate", "(Ljava/lang/String;)I");

		if (xuid == nullptr) {
			strcpy(g->Message, "Cannot find method ExecuteUpdate");
			return RC_FX;
		} // endif !xuid

	}	// endif xuid

	qry = env->NewStringUTF(sql);
	n = env->CallIntMethod(job, xuid, qry);
	env->DeleteLocalRef(qry);

	if (n < 0) {
		sprintf(g->Message, "Error executing %s: n = %d", sql, n);
		return RC_FX;
	} else {
		m_Ncol = 0;
		m_Aff = (int)n;			  // Affected rows
	} // endif n

	return RC_OK;
} // end of ExecuteUpdate

/***********************************************************************/
/*  Get the number of lines of the result set.                         */
/***********************************************************************/
int JDBConn::GetResultSize(char *sql, JDBCCOL *colp)
{
	int rc, n = 0;

	if ((rc = ExecuteQuery(sql)) != RC_OK)
		return -1;

	if ((rc = Fetch()) > 0)
		SetColumnValue(1, NULL, colp->GetValue());
	else
		return -2;

	if ((rc = Fetch()) != 0)
		return -3;
 
	m_Full = false;
	return colp->GetIntValue();
} // end of GetResultSize

/***********************************************************************/
/*  Execute a prepared statement.                                      */
/***********************************************************************/
int JDBConn::ExecuteSQL(void)
{
	int rc = RC_FX;
	PGLOBAL& g = m_G;

	if (xpid == nullptr) {
		// Get the methods used to execute a prepared statement
		xpid = env->GetMethodID(jdi, "ExecutePrep", "()I");

		if (xpid == nullptr) {
			strcpy(g->Message, "Cannot find method ExecutePrep");
			return rc;
		} // endif xpid

	} // endif xpid

	jint n = env->CallIntMethod(job, xpid);

	switch ((int)n) {
	case -1:
	case -2:
		strcpy(g->Message, "Exception error thrown while executing SQL");
		break;
	case -3:
		strcpy(g->Message, "SQL statement is not prepared");
		break;
	default:
		m_Aff = (int)n;
		rc = RC_OK;
	} // endswitch n

	return rc;
} // end of ExecuteSQL

/***********************************************************************/
/*  Set a parameter for inserting.                                     */
/***********************************************************************/
bool JDBConn::SetParam(JDBCCOL *colp)
{
	PGLOBAL&   g = m_G;
	int        rc = false;
	PVAL       val = colp->GetValue();
	jint       n, i = (jint)colp->GetRank();
	jshort     s;
	jlong      lg;
//jfloat     f;
	jdouble    d;
	jclass     dat;
	jobject    datobj;
	jstring    jst = nullptr;
	jthrowable exc;
	jmethodID  dtc, setid = nullptr;

	switch (val->GetType()) {
	case TYPE_STRING:
		setid = env->GetMethodID(jdi, "SetStringParm", "(ILjava/lang/String;)V");

		if (setid == nullptr) {
			strcpy(g->Message, "Cannot fing method SetStringParm");
			return true;
		}	// endif setid

		jst = env->NewStringUTF(val->GetCharValue());
		env->CallVoidMethod(job, setid, i, jst);
		break;
	case TYPE_INT:
		setid = env->GetMethodID(jdi, "SetIntParm", "(II)V");

		if (setid == nullptr) {
			strcpy(g->Message, "Cannot fing method SetIntParm");
			return true;
		}	// endif setid

		n = (jint)val->GetIntValue();
		env->CallVoidMethod(job, setid, i, n);
		break;
	case TYPE_TINY:
	case TYPE_SHORT:
		setid = env->GetMethodID(jdi, "SetShortParm", "(IS)V");

		if (setid == nullptr) {
			strcpy(g->Message, "Cannot fing method SetShortParm");
			return true;
		}	// endif setid

		s = (jshort)val->GetShortValue();
		env->CallVoidMethod(job, setid, i, s);
		break;
	case TYPE_BIGINT:
		setid = env->GetMethodID(jdi, "SetBigintParm", "(IJ)V");

		if (setid == nullptr) {
			strcpy(g->Message, "Cannot fing method SetBigintParm");
			return true;
		}	// endif setid

		lg = (jlong)val->GetBigintValue();
		env->CallVoidMethod(job, setid, i, lg);
		break;
	case TYPE_DOUBLE:
	case TYPE_DECIM:
		setid = env->GetMethodID(jdi, "SetDoubleParm", "(ID)V");

		if (setid == nullptr) {
			strcpy(g->Message, "Cannot fing method SetDoubleParm");
			return true;
		}	// endif setid

		d = (jdouble)val->GetFloatValue();
		env->CallVoidMethod(job, setid, i, d);
		break;
	case TYPE_DATE:
		if ((dat = env->FindClass("java/sql/Timestamp")) == nullptr) {
			strcpy(g->Message, "Cannot find Timestamp class");
			return true;
		} else if (!(dtc = env->GetMethodID(dat, "<init>", "(J)V"))) {
			strcpy(g->Message, "Cannot find Timestamp class constructor");
			return true;
		}	// endif's

		lg = (jlong)val->GetBigintValue() * 1000;

		if ((datobj = env->NewObject(dat, dtc, lg)) == nullptr) {
			strcpy(g->Message, "Cannot make Timestamp object");
			return true;
		} else if ((setid = env->GetMethodID(jdi, "SetTimestampParm", 
			             "(ILjava/sql/Timestamp;)V")) == nullptr) {
			strcpy(g->Message, "Cannot find method SetTimestampParm");
			return true;
		}	// endif setid

		env->CallVoidMethod(job, setid, i, datobj);
		break;
	default:
		sprintf(g->Message, "Parm type %d not supported", val->GetType());
		return true;
	}	// endswitch Type

	if ((exc = env->ExceptionOccurred()) != nullptr) {
		jboolean isCopy = false;
		jmethodID tid = env->GetMethodID(env->FindClass("java/lang/Object"), "toString", "()Ljava/lang/String;");
		jstring s = (jstring)env->CallObjectMethod(exc, tid);
		const char* utf = env->GetStringUTFChars(s, &isCopy);
		sprintf(g->Message, "SetParam: %s", utf);
		env->DeleteLocalRef(s);
		env->ExceptionClear();
		rc = true;
	} // endif exc

	if (jst)
		env->DeleteLocalRef(jst);

	return rc;
	} // end of SetParam

#if 0
	/***********************************************************************/
	/*  Get the list of Data Sources and set it in qrp.                    */
	/***********************************************************************/
	bool JDBConn::GetDataSources(PQRYRES qrp)
	{
		bool    rv = false;
		UCHAR  *dsn, *des;
		UWORD   dir = SQL_FETCH_FIRST;
		SWORD   n1, n2, p1, p2;
		PCOLRES crp1 = qrp->Colresp, crp2 = qrp->Colresp->Next;
		RETCODE rc;

		n1 = crp1->Clen;
		n2 = crp2->Clen;

		try {
			rc = SQLAllocEnv(&m_henv);

			if (!Check(rc))
				ThrowDJX(rc, "SQLAllocEnv"); // Fatal

			for (int i = 0; i < qrp->Maxres; i++) {
				dsn = (UCHAR*)crp1->Kdata->GetValPtr(i);
				des = (UCHAR*)crp2->Kdata->GetValPtr(i);
				rc = SQLDataSources(m_henv, dir, dsn, n1, &p1, des, n2, &p2);

				if (rc == SQL_NO_DATA_FOUND)
					break;
				else if (!Check(rc))
					ThrowDJX(rc, "SQLDataSources");

				qrp->Nblin++;
				dir = SQL_FETCH_NEXT;
			} // endfor i

		}
		catch (DJX *x) {
			sprintf(m_G->Message, "%s: %s", x->m_Msg, x->GetErrorMessage(0));
			rv = true;
		} // end try/catch

		Close();
		return rv;
	} // end of GetDataSources
#endif // 0

	/***********************************************************************/
	/*  Get the list of Drivers and set it in qrp.                         */
	/***********************************************************************/
	bool JDBConn::GetDrivers(PQRYRES qrp)
	{
		PSZ       sval;
		int       i, n, size;
		PCOLRES   crp;
		jstring   js;
		jmethodID gdid = env->GetMethodID(jdi, "GetDrivers", "([Ljava/lang/String;I)I");

		if (env->ExceptionCheck()) {
			strcpy(m_G->Message, "ERROR: method GetDrivers() not found!");
			env->ExceptionDescribe();
			env->ExceptionClear();
			return true;
		} // endif Check

		// Build the java string array
		jobjectArray s = env->NewObjectArray(4 * qrp->Maxres,
			env->FindClass("java/lang/String"), NULL);

		size = env->CallIntMethod(job, gdid, s, qrp->Maxres);

		for (i = 0, n = 0; i < size; i++) {
			crp = qrp->Colresp;
			js = (jstring)env->GetObjectArrayElement(s, n++);
			sval = (PSZ)env->GetStringUTFChars(js, 0);
			crp->Kdata->SetValue(sval, i);
			crp = crp->Next;
			js = (jstring)env->GetObjectArrayElement(s, n++);
			sval = (PSZ)env->GetStringUTFChars(js, 0);
			crp->Kdata->SetValue(sval, i);
			crp = crp->Next;
			js = (jstring)env->GetObjectArrayElement(s, n++);
			sval = (PSZ)env->GetStringUTFChars(js, 0);
			crp->Kdata->SetValue(sval, i);
			crp = crp->Next;
			js = (jstring)env->GetObjectArrayElement(s, n++);
			sval = (PSZ)env->GetStringUTFChars(js, 0);
			crp->Kdata->SetValue(sval, i);
		}	// endfor i

		// Not used anymore
		env->DeleteLocalRef(s);

		qrp->Nblin = size;
		return false;
	} // end of GetDrivers

	/**************************************************************************/
	/*  GetMetaData: constructs the result blocks containing the              */
	/*  description of all the columns of an SQL command.                     */
	/**************************************************************************/
	PQRYRES JDBConn::GetMetaData(PGLOBAL g, char *src)
	{
		static int  buftyp[] = {TYPE_STRING, TYPE_INT, TYPE_INT,
			                      TYPE_INT,    TYPE_INT};
		static XFLD fldtyp[] = {FLD_NAME,  FLD_TYPE, FLD_PREC,
			                      FLD_SCALE, FLD_NULL };
		static unsigned int length[] = {0, 6, 10, 6, 6};
		const char *name;
		int     len, qcol = 5;
		PQRYRES qrp = NULL;
		PCOLRES crp;
		USHORT  i;
		jint   *n;
		jstring label;
		jmethodID colid;
		int     rc = ExecSQLcommand(src);

		if (rc == RC_NF) {
			strcpy(g->Message, "Srcdef is not returning a result set");
			return NULL;
		} else if ((rc) == RC_FX) {
			return NULL;
		} else if (m_Ncol == 0) {
			strcpy(g->Message, "Invalid Srcdef");
			return NULL;
		} // endif's

		colid = env->GetMethodID(jdi, "ColumnDesc", "(I[I)Ljava/lang/String;");

		if (colid == nullptr) {
			strcpy(m_G->Message, "ERROR: method ColumnDesc() not found!");
			return NULL;
		} // endif colid

		// Build the java string array
		jintArray val = env->NewIntArray(4);

		if (val == nullptr) {
			strcpy(m_G->Message, "Cannot allocate jint array");
			return NULL;
		} // endif colid

		// Get max column name length
		len = GetMaxValue(5);
		length[0] = (len > 0) ? len + 1 : 128;

		/************************************************************************/
		/*  Allocate the structures used to refer to the result set.            */
		/************************************************************************/
		if (!(qrp = PlgAllocResult(g, qcol, m_Ncol, IDS_COLUMNS + 3,
			buftyp, fldtyp, length, false, true)))
			return NULL;

		// Some columns must be renamed
		for (i = 0, crp = qrp->Colresp; crp; crp = crp->Next)
			switch (++i) {
			case 3: crp->Name = "Precision"; break;
			case 4: crp->Name = "Scale";     break;
			case 5: crp->Name = "Nullable";  break;
		} // endswitch i

		/************************************************************************/
		/*  Now get the results into blocks.                                    */
		/************************************************************************/
		for (i = 0; i < m_Ncol; i++) {
			label = (jstring)env->CallObjectMethod(job, colid, i + 1, val);
			name = env->GetStringUTFChars(label, (jboolean)false);
			crp = qrp->Colresp;                    // Column_Name
			crp->Kdata->SetValue((char*)name, i);
			n = env->GetIntArrayElements(val, 0);
			crp = crp->Next;                       // Data_Type
			crp->Kdata->SetValue((int)n[0], i);
			crp = crp->Next;                       // Precision (length)
			crp->Kdata->SetValue((int)n[1], i);
			crp = crp->Next;                       // Scale
			crp->Kdata->SetValue((int)n[2], i);
			crp = crp->Next;                       // Nullable
			crp->Kdata->SetValue((int)n[3], i);
			qrp->Nblin++;
		} // endfor i

		/* Cleanup */
		env->ReleaseIntArrayElements(val, n, 0);
		Close();

		/************************************************************************/
		/*  Return the result pointer for use by GetData routines.              */
		/************************************************************************/
		return qrp;
	} // end of GetMetaData

	/***********************************************************************/
	/*  A helper class to split an optionally qualified table name into    */
	/*  components.                                                        */
	/*  These formats are understood:                                      */
	/*    "CatalogName.SchemaName.TableName"                               */
	/*    "SchemaName.TableName"                                           */
	/*    "TableName"                                                      */
	/***********************************************************************/
	class SQLQualifiedName
	{
		static const uint max_parts= 3;          // Catalog.Schema.Table
		MYSQL_LEX_STRING m_part[max_parts];
		char m_buf[512];

		void lex_string_set(MYSQL_LEX_STRING *S, char *str, size_t length)
		{
			S->str= str;
			S->length= length;
		} // end of lex_string_set

		void lex_string_shorten_down(MYSQL_LEX_STRING *S, size_t offs)
		{
			DBUG_ASSERT(offs <= S->length);
			S->str+= offs;
			S->length-= offs;
		} // end of lex_string_shorten_down

		/*********************************************************************/
		/*  Find the rightmost '.' delimiter and return the length           */
		/*  of the qualifier, including the rightmost '.' delimier.          */
		/*  For example, for the string {"a.b.c",5} it will return 4,        */
		/*  which is the length of the qualifier "a.b."                      */
		/*********************************************************************/
		size_t lex_string_find_qualifier(MYSQL_LEX_STRING *S)
		{
			size_t i;
			for (i= S->length; i > 0; i--)
			{
				if (S->str[i - 1] == '.')
				{
					S->str[i - 1]= '\0';
					return i;
				}
			}
			return 0;
		} // end of lex_string_find_qualifier

	public:
		/*********************************************************************/
		/*  Initialize to the given optionally qualified name.               */
		/*  NULL pointer in "name" is supported.                             */
		/*  name qualifier has precedence over schema.                       */
		/*********************************************************************/
		SQLQualifiedName(JCATPARM *cap)
		{
			const char *name = (const char *)cap->Tab;
			char       *db = (char *)cap->DB;
			size_t      len, i;

			// Initialize the parts
			for (i = 0; i < max_parts; i++)
				lex_string_set(&m_part[i], NULL, 0);

			if (name) {
				// Initialize the first (rightmost) part
				lex_string_set(&m_part[0], m_buf,
					strmake(m_buf, name, sizeof(m_buf) - 1) - m_buf);

				// Initialize the other parts, if exist. 
				for (i= 1; i < max_parts; i++) {
					if (!(len= lex_string_find_qualifier(&m_part[i - 1])))
						break;

					lex_string_set(&m_part[i], m_part[i - 1].str, len - 1);
					lex_string_shorten_down(&m_part[i - 1], len);
				} // endfor i

			} // endif name

			// If it was not specified, set schema as the passed db name
			if (db && !m_part[1].length)
				lex_string_set(&m_part[1], db, strlen(db));

		} // end of SQLQualifiedName

		char *ptr(uint i)
		{
			DBUG_ASSERT(i < max_parts);
			return (char *)(m_part[i].length ? m_part[i].str : NULL);
		} // end of ptr

		size_t length(uint i)
		{
			DBUG_ASSERT(i < max_parts);
			return m_part[i].length;
		} // end of length

	}; // end of class SQLQualifiedName

	/***********************************************************************/
	/*  Allocate recset and call SQLTables, SQLColumns or SQLPrimaryKeys.  */
	/***********************************************************************/
	int JDBConn::GetCatInfo(JCATPARM *cap)
	{
		PGLOBAL& g = m_G;
		void    *buffer;
		int      i;
		PSZ      fnc = "Unknown";
		uint     n, ncol;
		short    len, tp;
		int      crow = 0;
		PQRYRES  qrp = cap->Qrp;
		PCOLRES  crp;
		jboolean rc = false;
//	HSTMT    hstmt = NULL;
//	SQLLEN  *vl, *vlen = NULL;
		PVAL    *pval = NULL;
		char*   *pbuf = NULL;
		jobjectArray parms;
		jmethodID    catid;

		if (qrp->Maxres <= 0)
			return 0;				 			// 0-sized result"

		SQLQualifiedName name(cap);

		// Build the java string array
		parms = env->NewObjectArray(4, env->FindClass("java/lang/String"), NULL);
		env->SetObjectArrayElement(parms, 0, env->NewStringUTF(name.ptr(2)));
		env->SetObjectArrayElement(parms, 1, env->NewStringUTF(name.ptr(1)));
		env->SetObjectArrayElement(parms, 2, env->NewStringUTF(name.ptr(0)));

		if (cap->Pat)
			env->SetObjectArrayElement(parms, 3, env->NewStringUTF((const char*)cap->Pat));

		// Now do call the proper JDBC API
		switch (cap->Id) {
		case CAT_COL:
			fnc = "GetColumns";
			break;
		case CAT_TAB:
			fnc = "GetTables";
			break;
#if 0
		case CAT_KEY:
			fnc = "SQLPrimaryKeys";
			rc = SQLPrimaryKeys(hstmt, name.ptr(2), name.length(2),
				name.ptr(1), name.length(1),
				name.ptr(0), name.length(0));
			break;
		case CAT_STAT:
			fnc = "SQLStatistics";
			rc = SQLStatistics(hstmt, name.ptr(2), name.length(2),
				name.ptr(1), name.length(1),
				name.ptr(0), name.length(0),
				cap->Unique, cap->Accuracy);
			break;
		case CAT_SPC:
			ThrowDJX("SQLSpecialColumns not available yet");
#endif // 0
		default:
			sprintf(g->Message, "Invalid SQL function id");
			return -1;
		} // endswitch infotype

		catid = env->GetMethodID(jdi, fnc, "([Ljava/lang/String;)I");

		if (catid == nullptr) {
			sprintf(g->Message, "ERROR: method %s not found !", fnc);
			return -1;
		} // endif maxid

		// call method
		ncol = env->CallIntMethod(job, catid, parms);

		// Not used anymore
		env->DeleteLocalRef(parms);

		// n because we no more ignore the first column
		if ((n = qrp->Nbcol) > (int)ncol) {
			strcpy(g->Message, MSG(COL_NUM_MISM));
			return -1;
		} // endif n

		// Unconditional to handle STRBLK's
		pval = (PVAL *)PlugSubAlloc(g, NULL, n * sizeof(PVAL));
//	vlen = (SQLLEN *)PlugSubAlloc(g, NULL, n * sizeof(SQLLEN));
		pbuf = (char**)PlugSubAlloc(g, NULL, n * sizeof(char*));

		// Prepare retrieving column values
		for (n = 0, crp = qrp->Colresp; crp; crp = crp->Next) {
			if (!(tp = GetJDBCType(crp->Type))) {
				sprintf(g->Message, MSG(INV_COLUMN_TYPE), crp->Type, crp->Name);
				return -1;
			} // endif tp

			if (!(len = GetTypeSize(crp->Type, crp->Length))) {
				len = 255;           // for STRBLK's
				((STRBLK*)crp->Kdata)->SetSorted(true);
			} // endif len

			pval[n] = AllocateValue(g, crp->Type, len);

			if (crp->Type == TYPE_STRING) {
				pbuf[n] = (char*)PlugSubAlloc(g, NULL, len);
				buffer = pbuf[n];
			} else
				buffer = pval[n]->GetTo_Val();

			n++;
		} // endfor n

		// Now fetch the result
		// Extended fetch cannot be used because of STRBLK's
		for (i = 0; i < qrp->Maxres; i++) {
			if ((rc = Fetch(0)) == 0)
				break;
			else if (rc < 0)
				return -1;

			for (n = 0, crp = qrp->Colresp; crp; n++, crp = crp->Next) {
				SetColumnValue(n + 1, nullptr, pval[n]);
				crp->Kdata->SetValue(pval[n], i);
			}	// endfor n

		} // endfor i

		if (rc == RC_OK)
			qrp->Truncated = true;

		return i;
	} // end of GetCatInfo

	/***********************************************************************/
	/*  Allocate a CONNECT result structure from the JDBC result.          */
	/***********************************************************************/
	PQRYRES JDBConn::AllocateResult(PGLOBAL g)
	{
		bool         uns;
		PJDBCCOL     colp;
		PCOLRES     *pcrp, crp;
		PQRYRES      qrp;

		if (!m_Rows) {
			strcpy(g->Message, "Void result");
			return NULL;
		} // endif m_Res

		/*********************************************************************/
		/*  Allocate the result storage for future retrieval.                */
		/*********************************************************************/
		qrp = (PQRYRES)PlugSubAlloc(g, NULL, sizeof(QRYRES));
		pcrp = &qrp->Colresp;
		qrp->Continued = FALSE;
		qrp->Truncated = FALSE;
		qrp->Info = FALSE;
		qrp->Suball = TRUE;
		qrp->BadLines = 0;
		qrp->Maxsize = m_Rows;
		qrp->Maxres = m_Rows;
		qrp->Nbcol = 0;
		qrp->Nblin = 0;
		qrp->Cursor = 0;

		for (colp = (PJDBCCOL)m_Tdb->Columns; colp;
			   colp = (PJDBCCOL)colp->GetNext())
			if (!colp->IsSpecial()) {
				*pcrp = (PCOLRES)PlugSubAlloc(g, NULL, sizeof(COLRES));
				crp = *pcrp;
				pcrp = &crp->Next;
				memset(crp, 0, sizeof(COLRES));
				crp->Ncol = ++qrp->Nbcol;
				crp->Name = colp->GetName();
				crp->Type = colp->GetResultType();
				crp->Prec = colp->GetScale();
				crp->Length = colp->GetLength();
				crp->Clen = colp->GetValue()->GetClen();
				uns = colp->IsUnsigned();

				if (!(crp->Kdata = AllocValBlock(g, NULL, crp->Type, m_Rows,
					crp->Clen, 0, FALSE, TRUE, uns))) {
					sprintf(g->Message, MSG(INV_RESULT_TYPE),
						GetFormatType(crp->Type));
					return NULL;
				} // endif Kdata

				if (!colp->IsNullable())
					crp->Nulls = NULL;
				else {
					crp->Nulls = (char*)PlugSubAlloc(g, NULL, m_Rows);
					memset(crp->Nulls, ' ', m_Rows);
				} // endelse Nullable

				colp->SetCrp(crp);
			} // endif colp

		*pcrp = NULL;
		//qrp->Nblin = n;
		return qrp;
	} // end of AllocateResult
