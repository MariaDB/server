/***********************************************************************/
/*  Name: ODBCONN.CPP  Version 2.4                                     */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1998-2021    */
/*                                                                     */
/*  This file contains the ODBC connection classes functions.          */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include <my_global.h>
#include <m_string.h>
#if defined(_WIN32)
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
#include "tabext.h"
#include "odbccat.h"
#include "tabodbc.h"
#include "plgcnx.h"                       // For DB types
#include "resource.h"
#include "valblk.h"
#include "osutil.h"


#if defined(_WIN32)
/***********************************************************************/
/*  For dynamic load of ODBC32.DLL                                     */
/***********************************************************************/
#pragma comment(lib, "odbc32.lib")
extern "C" HINSTANCE s_hModule;           // Saved module handle
#endif // _WIN32

TYPCONV GetTypeConv();
int GetConvSize();
void OdbcClose(PGLOBAL g, PFBLOCK fp);

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
/*  GetSQLType: returns the SQL_TYPE corresponding to a PLG type.      */
/***********************************************************************/
static short GetSQLType(int type)
  {
  short tp = SQL_TYPE_NULL;

  switch (type) {
    case TYPE_STRING: tp = SQL_CHAR;      break;
    case TYPE_SHORT:  tp = SQL_SMALLINT;  break;
    case TYPE_INT:    tp = SQL_INTEGER;   break;
    case TYPE_DATE:   tp = SQL_TIMESTAMP; break;
    case TYPE_BIGINT: tp = SQL_BIGINT;    break;                //  (-5)
    case TYPE_DOUBLE: tp = SQL_DOUBLE;    break;
    case TYPE_TINY:   tp = SQL_TINYINT;   break;
    case TYPE_DECIM:  tp = SQL_DECIMAL;   break;
    } // endswitch type

  return tp;
  } // end of GetSQLType

/***********************************************************************/
/*  GetSQLCType: returns the SQL_C_TYPE corresponding to a PLG type.   */
/***********************************************************************/
static int GetSQLCType(int type)
  {
  int tp = SQL_TYPE_NULL;

  switch (type) {
    case TYPE_STRING: tp = SQL_C_CHAR;      break;
    case TYPE_SHORT:  tp = SQL_C_SHORT;     break;
    case TYPE_INT:    tp = SQL_C_LONG;      break;
    case TYPE_DATE:   tp = SQL_C_TIMESTAMP; break;
    case TYPE_BIGINT: tp = SQL_C_SBIGINT;   break;
    case TYPE_DOUBLE: tp = SQL_C_DOUBLE;    break;
    case TYPE_TINY :  tp = SQL_C_TINYINT;   break;
//#if (ODBCVER >= 0x0300)
//    case TYPE_DECIM:  tp = SQL_C_NUMERIC;   break;  (CRASH!!!)
//#else
    case TYPE_DECIM:  tp = SQL_C_CHAR;      break;
//#endif

    } // endswitch type

  return tp;
  } // end of GetSQLCType

/***********************************************************************/
/*  TranslateSQLType: translate a SQL Type to a PLG type.              */
/***********************************************************************/
int TranslateSQLType(int stp, int prec, int& len, char& v, bool& w)
  {
  int type;

  switch (stp) {
    case SQL_WVARCHAR:                      //  (-9)
      w = true;
    case SQL_VARCHAR:                       //   12
      v = 'V';
      type = TYPE_STRING;
      break;
    case SQL_WCHAR:                         //  (-8)
      w = true;
    case SQL_CHAR:                          //    1
      type = TYPE_STRING;
      break;
    case SQL_WLONGVARCHAR:                  // (-10)
      w = true;
    case SQL_LONGVARCHAR:                   //  (-1)
			if (GetTypeConv() == TPC_YES || GetTypeConv() == TPC_FORCE) {
				v = 'V';
				type = TYPE_STRING;
				len = (len) ? MY_MIN(abs(len), GetConvSize()) : GetConvSize();
			} else
				type = TYPE_ERROR;

      break;
    case SQL_NUMERIC:                       //    2
    case SQL_DECIMAL:                       //    3
//    type = (prec || len > 20) ? TYPE_DOUBLE
//         : (len > 10) ? TYPE_BIGINT : TYPE_INT;
      type = TYPE_DECIM;
      break;
    case SQL_INTEGER:                       //    4
      type = TYPE_INT;
      break;
    case SQL_SMALLINT:                      //    5
      type = TYPE_SHORT;
      break;
    case SQL_TINYINT:                       //  (-6)
    case SQL_BIT:                           //  (-7)
      type = TYPE_TINY;
      break;
    case SQL_FLOAT:                         //    6
    case SQL_REAL:                          //    7
    case SQL_DOUBLE:                        //    8
      type = TYPE_DOUBLE;
      break;
    case SQL_DATETIME:                      //    9
      type = TYPE_DATE;
      len = 19;
      break;
    case SQL_TYPE_DATE:                     //   91
      type = TYPE_DATE;
      len = 10;
      v = 'D';
      break;
    case SQL_INTERVAL:                      //   10
    case SQL_TYPE_TIME:                     //   92
      type = TYPE_STRING;
      len = 8 + ((prec) ? (prec+1) : 0);
      v = 'T';
      break;
    case SQL_TIMESTAMP:                     //   11
    case SQL_TYPE_TIMESTAMP:                //   93
      type = TYPE_DATE;
      len = 19 + ((prec) ? (prec+1) : 0);
      v = 'S';
      break;
    case SQL_BIGINT:                        //  (-5)
      type = TYPE_BIGINT;
      break;
    case SQL_BINARY:                        //  (-2)
    case SQL_VARBINARY:                     //  (-3)
    case SQL_LONGVARBINARY:                 //  (-4)
			if (GetTypeConv() == TPC_FORCE) {
				v = 'V';
				type = TYPE_STRING;
				len = (len) ? MY_MIN(abs(len), GetConvSize()) : GetConvSize();
			}	else
				type = TYPE_ERROR;

			break;
		case SQL_GUID:                          // (-11)
			type = TYPE_STRING;
			len = 36;
			break;
		case SQL_UNKNOWN_TYPE:                  //    0
		default:
      type = TYPE_ERROR;
      len = 0;
    } // endswitch type

  return type;
  } // end of TranslateSQLType

#if defined(PROMPT_OK)
/***********************************************************************/
/*  ODBCCheckConnection: Check completeness of connection string.      */
/***********************************************************************/
char *ODBCCheckConnection(PGLOBAL g, char *dsn, int cop)
  {
  char    *newdsn, dir[_MAX_PATH], buf[_MAX_PATH];
  int      rc;
  DWORD    options = ODBConn::openReadOnly;
  ODBConn *ocp = new(g) ODBConn(g, NULL);

  (void) getcwd(dir, sizeof(dir) - 1);

  switch (cop) {
    case 1: options |= ODBConn::forceOdbcDialog; break;
    case 2: options |= ODBConn::noOdbcDialog;    break;
    } // endswitch cop

  if (ocp->Open(dsn, options) < 1)
    newdsn = NULL;
  else
    newdsn = ocp->GetConnect();

  (void) getcwd(buf, sizeof(buf) - 1);

  // Some data sources change the current directory
  if (strcmp(dir, buf))
    rc = chdir(dir);

  ocp->Close();
  return newdsn;         // Return complete connection string
  } // end of ODBCCheckConnection
#endif   // PROMPT_OK

/***********************************************************************/
/*  Allocate the structure used to refer to the result set.            */
/***********************************************************************/
static CATPARM *AllocCatInfo(PGLOBAL g, CATINFO fid, PCSZ db,
	                                      PCSZ tab, PQRYRES qrp)
{
	size_t   i, m, n;
	CATPARM *cap;

#if defined(_DEBUG)
	assert(qrp);
#endif

	try {
		m = (size_t)qrp->Maxres;
		n = (size_t)qrp->Nbcol;
		cap = (CATPARM *)PlugSubAlloc(g, NULL, sizeof(CATPARM));
		memset(cap, 0, sizeof(CATPARM));
		cap->Id = fid;
		cap->Qrp = qrp;
		cap->DB = db;
		cap->Tab = tab;
		cap->Vlen = (SQLLEN* *)PlugSubAlloc(g, NULL, n * sizeof(SQLLEN *));

		for (i = 0; i < n; i++)
			cap->Vlen[i] = (SQLLEN *)PlugSubAlloc(g, NULL, m * sizeof(SQLLEN));

		cap->Status = (UWORD *)PlugSubAlloc(g, NULL, m * sizeof(UWORD));

	} catch (int n) {
		htrc("Exeption %d: %s\n", n, g->Message);
		cap = NULL;
	} catch (const char *msg) {
		htrc(g->Message, msg);
		printf("%s\n", g->Message);
		cap = NULL;
	} // end catch

	return cap;
} // end of AllocCatInfo

#if 0
/***********************************************************************/
/*  Check for nulls and reset them to Null (?) values.                 */
/***********************************************************************/
static void ResetNullValues(CATPARM *cap)
  {
  int      i, n, ncol;
  PCOLRES  crp;
  PQRYRES  qrp = cap->Qrp;

#if defined(_DEBUG)
  assert(qrp);
#endif

  ncol = qrp->Nbcol;

  for (i = 0, crp = qrp->Colresp; i < ncol && crp; i++, crp = crp->Next)
    for (n = 0; n < qrp->Nblin; n++)
      if (cap->Vlen[i][n] == SQL_NULL_DATA)
        crp->Kdata->Reset(n);

  } // end of ResetNullValues
#endif

/***********************************************************************/
/*  Close an ODBC table after a thrown error (called by PlugCloseFile) */
/***********************************************************************/
void OdbcClose(PGLOBAL g, PFBLOCK fp) {
	((ODBConn*)fp->File)->Close();
}	// end of OdbcClose

/***********************************************************************/
/*  ODBCColumns: constructs the result blocks containing all columns   */
/*  of an ODBC table that will be retrieved by GetData commands.       */
/***********************************************************************/
PQRYRES ODBCColumns(PGLOBAL g, PCSZ dsn, PCSZ db, PCSZ table,
	                  PCSZ colpat, int maxres, bool info, POPARM sop)
  {
  int  buftyp[] = {TYPE_STRING, TYPE_STRING, TYPE_STRING, TYPE_STRING,
                   TYPE_SHORT,  TYPE_STRING, TYPE_INT,    TYPE_INT,
                   TYPE_SHORT,  TYPE_SHORT,  TYPE_SHORT,  TYPE_STRING};
  XFLD fldtyp[] = {FLD_CAT,   FLD_SCHEM,    FLD_TABNAME, FLD_NAME,
                   FLD_TYPE,  FLD_TYPENAME, FLD_PREC,    FLD_LENGTH,
                   FLD_SCALE, FLD_RADIX,    FLD_NULL,    FLD_REM};
  unsigned int length[] = {0, 0, 0, 0, 6, 0, 10, 10, 6, 6, 6, 0};
	bool     b[] = {true,true,false,false,false,false,false,false,true,true,false,true};
  int      i, n, ncol = 12;
	PCOLRES  crp;
	PQRYRES  qrp;
  CATPARM *cap;
  ODBConn *ocp = NULL;

  /************************************************************************/
  /*  Do an evaluation of the result size.                                */
  /************************************************************************/
  if (!info) {
    ocp = new(g) ODBConn(g, NULL);

    if (ocp->Open(dsn, sop, 10) < 1)  // openReadOnly + noODBCdialog
      return NULL;

    if (table && !strchr(table, '%')) {
      // We fix a MySQL limit because some data sources return 32767
      n = ocp->GetMaxValue(SQL_MAX_COLUMNS_IN_TABLE);
      maxres = (n) ? MY_MIN(n, 4096) : 4096;
    } else if (!maxres)
      maxres = 20000;

//  n = ocp->GetMaxValue(SQL_MAX_CATALOG_NAME_LEN);
//  length[0] = (n) ? (n + 1) : 0;
//  n = ocp->GetMaxValue(SQL_MAX_SCHEMA_NAME_LEN);
//  length[1] = (n) ? (n + 1) : 0;
//  n = ocp->GetMaxValue(SQL_MAX_TABLE_NAME_LEN);
//  length[2] = (n) ? (n + 1) : 0;
    n = ocp->GetMaxValue(SQL_MAX_COLUMN_NAME_LEN);
    length[3] = (n) ? (n + 1) : 128;
  } else {                 // Info table
    maxres = 0;
    length[0] = 128;
    length[1] = 128;
    length[2] = 128;
    length[3] = 128;
    length[5] = 30;
    length[11] = 255;
  } // endif ocp

  if (trace(1))
    htrc("ODBCColumns: max=%d len=%d,%d,%d,%d\n",
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

  if (trace(1))
    htrc("Getting col results ncol=%d\n", qrp->Nbcol);

  if (!(cap = AllocCatInfo(g, CAT_COL, db, table, qrp)))
    return NULL;

  cap->Pat = colpat;

  /************************************************************************/
  /*  Now get the results into blocks.                                    */
  /************************************************************************/
  if ((n = ocp->GetCatInfo(cap)) >= 0) {
    qrp->Nblin = n;
//  ResetNullValues(cap);

    if (trace(1))
      htrc("Columns: NBCOL=%d NBLIN=%d\n", qrp->Nbcol, qrp->Nblin);

  } else
    qrp = NULL;

  /* Cleanup */
  ocp->Close();

  /************************************************************************/
  /*  Return the result pointer for use by GetData routines.              */
  /************************************************************************/
  return qrp;
  } // end of ODBCColumns

/**************************************************************************/
/*  ODBCSrcCols: constructs the result blocks containing the              */
/*  description of all the columns of a Srcdef option.                    */
/**************************************************************************/
PQRYRES ODBCSrcCols(PGLOBAL g, char *dsn, char *src, POPARM sop)
  {
	char    *sqry;
  ODBConn *ocp = new(g) ODBConn(g, NULL);

  if (ocp->Open(dsn, sop, 10) < 1)   // openReadOnly + noOdbcDialog
    return NULL;

	if (strstr(src, "%s")) {
		// Place holder for an eventual where clause
		sqry = (char*)PlugSubAlloc(g, NULL, strlen(src) + 3);
		sprintf(sqry, src, "1=1", "1=1");			 // dummy where clause
	} else
		sqry = src;

  return ocp->GetMetaData(g, dsn, sqry);
  } // end of ODBCSrcCols

#if 0
/**************************************************************************/
/* MyODBCCols: returns column info as required by ha_connect::pre_create. */
/**************************************************************************/
PQRYRES MyODBCCols(PGLOBAL g, char *dsn, char *tab, bool info)
  {
//  int      i, type, len, prec;
  bool     w = false;
//  PCOLRES  crp, crpt, crpl, crpp;
  PQRYRES  qrp;
  ODBConn *ocp;

  /**********************************************************************/
  /*  Open the connection with the ODBC data source.                    */
  /**********************************************************************/
  if (!info) {
    ocp = new(g) ODBConn(g, NULL);

    if (ocp->Open(dsn, 2) < 1)        // 2 is openReadOnly
      return NULL;

  } else
    ocp = NULL;

  /**********************************************************************/
  /*  Get the information about the ODBC table columns.                 */
  /**********************************************************************/
  if ((qrp = ODBCColumns(g, ocp, dsn, NULL, tab, 0, NULL)) && ocp)
    dsn = ocp->GetConnect();        // Complete connect string

  /************************************************************************/
  /*  Close the local connection.                                         */
  /************************************************************************/
  if (ocp)
    ocp->Close();

  if (!qrp)
    return NULL;             // Error in ODBCColumns

  /************************************************************************/
  /*  Keep only the info used by ha_connect::pre_create.                  */
  /************************************************************************/
  qrp->Colresp = qrp->Colresp->Next->Next;  // Skip Schema and Table names

  crpt = qrp->Colresp->Next;                // SQL type
  crpl = crpt->Next->Next;                  // Length
  crpp = crpl->Next->Next;                  // Decimals

  for (int i = 0; i < qrp->Nblin; i++) {
    // Types must be PLG types, not SQL types
    type = crpt->Kdata->GetIntValue(i);
    len  = crpl->Kdata->GetIntValue(i);
    prec = crpp->Kdata->GetIntValue(i);
    type = TranslateSQLType(type, prec, len, w);
    crpt->Kdata->SetValue(type, i);

    // Some data sources do not count prec in length
    if (type == TYPE_DOUBLE)
      len += (prec + 2);                    // To be safe

    // Could have been changed for blobs or numeric
    crpl->Kdata->SetValue(len, i);          
    } // endfor i

  crpp->Next = crpp->Next->Next->Next;      // Should be Remark

  // Renumber crp's for flag comparison
  for (i = 0, crp = qrp->Colresp; crp; crp = crp->Next)
    crp->Ncol = ++i;

  qrp->Nbcol = i;             // Should be 7; was 11, skipped 4
  return qrp;
  } // end of MyODBCCols
#endif // 0

/*************************************************************************/
/*  ODBCDrivers: constructs the result blocks containing all ODBC        */
/*  drivers available on the local host.                                 */
/*  Called with info=true to have result column names.                   */
/*************************************************************************/
PQRYRES ODBCDrivers(PGLOBAL g, int maxres, bool info)
  {
  int      buftyp[] = {TYPE_STRING, TYPE_STRING};
  XFLD     fldtyp[] = {FLD_NAME, FLD_REM};
  unsigned int length[] = {128, 256};
	bool     b[] = {false, true};
	int      i, ncol = 2;
	PCOLRES  crp;
	PQRYRES  qrp;
  ODBConn *ocp = NULL;

  /************************************************************************/
  /*  Do an evaluation of the result size.                                */
  /************************************************************************/
  if (!info) {
    ocp = new(g) ODBConn(g, NULL);

    if (!maxres)
      maxres = 256;         // Estimated max number of drivers

  } else
    maxres = 0;

  if (trace(1))
    htrc("ODBCDrivers: max=%d len=%d\n", maxres, length[0]);

  /************************************************************************/
  /*  Allocate the structures used to refer to the result set.            */
  /************************************************************************/
  qrp = PlgAllocResult(g, ncol, maxres, IDS_DRIVER, 
                          buftyp, fldtyp, length, false, true);

	for (i = 0, crp = qrp->Colresp; crp; i++, crp = crp->Next)
		if (b[i])
			crp->Kdata->SetNullable(true);

	/************************************************************************/
  /*  Now get the results into blocks.                                    */
  /************************************************************************/
  if (!info && qrp && ocp->GetDrivers(qrp))
    qrp = NULL;

  /************************************************************************/
  /*  Return the result pointer for use by GetData routines.              */
  /************************************************************************/
  return qrp;
  } // end of ODBCDrivers

/*************************************************************************/
/*  ODBCDataSources: constructs the result blocks containing all ODBC    */
/*  data sources available on the local host.                            */
/*  Called with info=true to have result column names.                   */
/*************************************************************************/
PQRYRES ODBCDataSources(PGLOBAL g, int maxres, bool info)
  {
  int      buftyp[] = {TYPE_STRING, TYPE_STRING};
  XFLD     fldtyp[] = {FLD_NAME, FLD_REM};
  unsigned int length[] = {0, 256};
	bool     b[] = {false, true};
	int      i, n = 0, ncol = 2;
	PCOLRES  crp;
	PQRYRES  qrp;
  ODBConn *ocp = NULL;

  /************************************************************************/
  /*  Do an evaluation of the result size.                                */
  /************************************************************************/
  if (!info) {
    ocp = new(g) ODBConn(g, NULL);
    n = ocp->GetMaxValue(SQL_MAX_DSN_LENGTH);
    length[0] = (n) ? (n + 1) : 256;

    if (!maxres)
      maxres = 512;         // Estimated max number of data sources

  } else {
    length[0] = 256;
    maxres = 0;
  } // endif info

  if (trace(1))
    htrc("ODBCDataSources: max=%d len=%d\n", maxres, length[0]);

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
  if (!info && qrp && ocp->GetDataSources(qrp))
    qrp = NULL;

  /************************************************************************/
  /*  Return the result pointer for use by GetData routines.              */
  /************************************************************************/
  return qrp;
  } // end of ODBCDataSources

/**************************************************************************/
/*  ODBCTables: constructs the result blocks containing all tables in     */
/*  an ODBC database that will be retrieved by GetData commands.          */
/**************************************************************************/
PQRYRES ODBCTables(PGLOBAL g, PCSZ dsn, PCSZ db, PCSZ tabpat, PCSZ tabtyp,
	                 int maxres, bool info, POPARM sop)
  {
  int      buftyp[] = {TYPE_STRING, TYPE_STRING, TYPE_STRING,
                       TYPE_STRING, TYPE_STRING};
  XFLD     fldtyp[] = {FLD_CAT,  FLD_SCHEM, FLD_NAME,
                       FLD_TYPE, FLD_REM};
  unsigned int length[] = {0, 0, 0, 16, 0};
	bool     b[] ={ true, true, false, false, true };
	int      i, n, ncol = 5;
	PCOLRES  crp;
	PQRYRES  qrp;
  CATPARM *cap;
  ODBConn *ocp = NULL;

  /************************************************************************/
  /*  Do an evaluation of the result size.                                */
  /************************************************************************/
  if (!info) {
    /**********************************************************************/
    /*  Open the connection with the ODBC data source.                    */
    /**********************************************************************/
    ocp = new(g) ODBConn(g, NULL);

    if (ocp->Open(dsn, sop, 2) < 1)        // 2 is openReadOnly
      return NULL;

    if (!maxres)
      maxres = 10000;                 // This is completely arbitrary

//  n = ocp->GetMaxValue(SQL_MAX_CATALOG_NAME_LEN);
//  length[0] = (n) ? (n + 1) : 0;
//  n = ocp->GetMaxValue(SQL_MAX_SCHEMA_NAME_LEN);
//  length[1] = (n) ? (n + 1) : 0;
    n = ocp->GetMaxValue(SQL_MAX_TABLE_NAME_LEN);
    length[2] = (n) ? (n + 1) : 128;
  } else {
    maxres = 0;
    length[0] = 128;
    length[1] = 128;
    length[2] = 128;
    length[4] = 255;
  } // endif info

  if (trace(1))
    htrc("ODBCTables: max=%d len=%d,%d\n", maxres, length[0], length[1]);

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

	cap->Pat = tabtyp;

  if (trace(1))
    htrc("Getting table results ncol=%d\n", cap->Qrp->Nbcol);

  /************************************************************************/
  /*  Now get the results into blocks.                                    */
  /************************************************************************/
  if ((n = ocp->GetCatInfo(cap)) >= 0) {
    qrp->Nblin = n;
//  ResetNullValues(cap);

    if (trace(1))
      htrc("Tables: NBCOL=%d NBLIN=%d\n", qrp->Nbcol, qrp->Nblin);

  } else
    qrp = NULL;

  /************************************************************************/
  /*  Close any local connection.                                         */
  /************************************************************************/
  ocp->Close();

  /************************************************************************/
  /*  Return the result pointer for use by GetData routines.              */
  /************************************************************************/
  return qrp;
  } // end of ODBCTables

#if 0                           // Currently not used by CONNECT
/**************************************************************************/
/*  PrimaryKeys: constructs the result blocks containing all the          */
/*  ODBC catalog information concerning primary keys.                     */
/**************************************************************************/
PQRYRES ODBCPrimaryKeys(PGLOBAL g, ODBConn *op, char *dsn, char *table)
  {
  static int buftyp[] = {TYPE_STRING, TYPE_STRING, TYPE_STRING,
                         TYPE_STRING, TYPE_SHORT,  TYPE_STRING};
  static unsigned int length[] = {0, 0, 0, 0, 6, 128};
  int      n, ncol = 5;
  int     maxres;
  PQRYRES  qrp;
  CATPARM *cap;
  ODBConn *ocp = op;

  if (!op) {
    /**********************************************************************/
    /*  Open the connection with the ODBC data source.                    */
    /**********************************************************************/
    ocp = new(g) ODBConn(g, NULL);

    if (ocp->Open(dsn, 2) < 1)        // 2 is openReadOnly
      return NULL;

    } // endif op

  /************************************************************************/
  /*  Do an evaluation of the result size.                                */
  /************************************************************************/
  n = ocp->GetMaxValue(SQL_MAX_COLUMNS_IN_TABLE);
  maxres = (n) ? (int)n : 250;
  n = ocp->GetMaxValue(SQL_MAX_CATALOG_NAME_LEN);
  length[0] = (n) ? (n + 1) : 128;
  n = ocp->GetMaxValue(SQL_MAX_SCHEMA_NAME_LEN);
  length[1] = (n) ? (n + 1) : 128;
  n = ocp->GetMaxValue(SQL_MAX_TABLE_NAME_LEN);
  length[2] = (n) ? (n + 1) : 128;
  n = ocp->GetMaxValue(SQL_MAX_COLUMN_NAME_LEN);
  length[3] = (n) ? (n + 1) : 128;

  if (trace(1))
    htrc("ODBCPrimaryKeys: max=%d len=%d,%d,%d\n",
         maxres, length[0], length[1], length[2]);

  /************************************************************************/
  /*  Allocate the structure used to refer to the result set.             */
  /************************************************************************/
  qrp = PlgAllocResult(g, ncol, maxres, IDS_PKEY,
                          buftyp, NULL, length, false, true);

  if (trace(1))
    htrc("Getting pkey results ncol=%d\n", qrp->Nbcol);

  cap = AllocCatInfo(g, CAT_KEY, NULL, table, qrp);

  /************************************************************************/
  /*  Now get the results into blocks.                                    */
  /************************************************************************/
  if ((n = ocp->GetCatInfo(cap)) >= 0) {
    qrp->Nblin = n;
//  ResetNullValues(cap);

    if (trace(1))
      htrc("PrimaryKeys: NBCOL=%d NBLIN=%d\n", qrp->Nbcol, qrp->Nblin);

  } else
    qrp = NULL;

  /************************************************************************/
  /*  Close any local connection.                                         */
  /************************************************************************/
  if (!op)
    ocp->Close();

  /************************************************************************/
  /*  Return the result pointer for use by GetData routines.              */
  /************************************************************************/
  return qrp;
  } // end of ODBCPrimaryKeys

/**************************************************************************/
/*  Statistics: constructs the result blocks containing statistics        */
/*  about one or several tables to be retrieved by GetData commands.      */
/**************************************************************************/
PQRYRES ODBCStatistics(PGLOBAL g, ODBConn *op, char *dsn, char *pat,
                                               int un, int acc)
  {
  static int buftyp[] = {TYPE_STRING,
                         TYPE_STRING, TYPE_STRING, TYPE_SHORT, TYPE_STRING,
                         TYPE_STRING, TYPE_SHORT,  TYPE_SHORT, TYPE_STRING,
                         TYPE_STRING, TYPE_INT,   TYPE_INT,  TYPE_STRING};
  static unsigned int length[] = {0, 0, 0 ,6 ,0 ,0 ,6 ,6 ,0 ,2 ,10 ,10 ,128};
  int      n, ncol = 13;
  int     maxres;
  PQRYRES  qrp;
  CATPARM *cap;
  ODBConn *ocp = op;

  if (!op) {
    /**********************************************************************/
    /*  Open the connection with the ODBC data source.                    */
    /**********************************************************************/
    ocp = new(g) ODBConn(g, NULL);

    if (ocp->Open(dsn, 2) < 1)        // 2 is openReadOnly
      return NULL;

    } // endif op

  /************************************************************************/
  /*  Do an evaluation of the result size.                                */
  /************************************************************************/
  n = 1 + ocp->GetMaxValue(SQL_MAX_COLUMNS_IN_INDEX);
  maxres = (n) ? (int)n : 32;
  n = ocp->GetMaxValue(SQL_MAX_SCHEMA_NAME_LEN);
  length[1] = (n) ? (n + 1) : 128;
  n = ocp->GetMaxValue(SQL_MAX_TABLE_NAME_LEN);
  length[2] = length[5] = (n) ? (n + 1) : 128;
  n = ocp->GetMaxValue(SQL_MAX_CATALOG_NAME_LEN);
  length[0] = length[4] = (n) ? (n + 1) : length[2];
  n = ocp->GetMaxValue(SQL_MAX_COLUMN_NAME_LEN);
  length[7] = (n) ? (n + 1) : 128;

  if (trace(1))
    htrc("SemStatistics: max=%d pat=%s\n", maxres, SVP(pat));

  /************************************************************************/
  /*  Allocate the structure used to refer to the result set.             */
  /************************************************************************/
  qrp = PlgAllocResult(g, ncol, maxres, IDS_STAT,
                          buftyp, NULL, length, false, true);

  if (trace(1))
    htrc("Getting stat results ncol=%d\n", qrp->Nbcol);

  cap = AllocCatInfo(g, CAT_STAT, NULL, pat, qrp);
  cap->Unique = (un < 0) ? SQL_INDEX_UNIQUE : (UWORD)un;
  cap->Accuracy = (acc < 0) ? SQL_QUICK : (UWORD)acc;

  /************************************************************************/
  /*  Now get the results into blocks.                                    */
  /************************************************************************/
  if ((n = ocp->GetCatInfo(cap)) >= 0) {
    qrp->Nblin = n;
//  ResetNullValues(cap);

    if (trace(1))
      htrc("Statistics: NBCOL=%d NBLIN=%d\n", qrp->Nbcol, qrp->Nblin);

  } else
    qrp = NULL;

  /************************************************************************/
  /*  Close any local connection.                                         */
  /************************************************************************/
  if (!op)
    ocp->Close();

  /************************************************************************/
  /*  Return the result pointer for use by GetData routines.              */
  /************************************************************************/
  return qrp;
  } // end of Statistics
#endif // 0

/***********************************************************************/
/*  Implementation of DBX class.                                       */
/***********************************************************************/
DBX::DBX(RETCODE rc, PCSZ msg)
  {
  m_RC = rc;
  m_Msg = msg;

  for (int i = 0; i < MAX_NUM_OF_MSG; i++)
    m_ErrMsg[i] = NULL;

  } // end of DBX constructor

/***********************************************************************/
/*  This function is called by ThrowDBX.                               */
/***********************************************************************/
bool DBX::BuildErrorMessage(ODBConn* pdb, HSTMT hstmt)
  {
  if (pdb) {
    SWORD   len;
    RETCODE rc;
    UCHAR   msg[SQL_MAX_MESSAGE_LENGTH + 1];
    UCHAR   state[SQL_SQLSTATE_SIZE + 1];
    SDWORD  native;
    PGLOBAL g = pdb->m_G;

    rc = SQLError(pdb->m_henv, pdb->m_hdbc, hstmt, state,
                  &native, msg, SQL_MAX_MESSAGE_LENGTH - 1, &len);

    if (rc == SQL_NO_DATA_FOUND)
      return false;
    else if (rc != SQL_INVALID_HANDLE) {
    // Skip non-errors
      for (int i = 0; i < MAX_NUM_OF_MSG
              && (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
              && strcmp((char*)state, "00000"); i++) {
        m_ErrMsg[i] = (PSZ)PlugDup(g, (char*)msg);

        if (trace(1))
          htrc("%s: %s, Native=%d\n", state, msg, native);

        rc = SQLError(pdb->m_henv, pdb->m_hdbc, hstmt, state,
                      &native, msg, SQL_MAX_MESSAGE_LENGTH - 1, &len);

        } // endfor i

      return true;
    } else {
      snprintf((char*)msg, SQL_MAX_MESSAGE_LENGTH + 1, "%s: %s", m_Msg,
               MSG(BAD_HANDLE_VAL));
      m_ErrMsg[0] = (PSZ)PlugDup(g, (char*)msg);

      if (trace(1))
        htrc("%s: rc=%hd\n", SVP(m_ErrMsg[0]), m_RC); 

      return true;
    } // endif rc

  } else
    m_ErrMsg[0] = "No connexion address provided";

  if (trace(1))
    htrc("%s: rc=%hd (%s)\n", SVP(m_Msg), m_RC, SVP(m_ErrMsg[0])); 

  return true;
  } // end of BuildErrorMessage

const char *DBX::GetErrorMessage(int i)
  {
    if (i < 0 || i >= MAX_NUM_OF_MSG)
      return "No ODBC error";
    else if (m_ErrMsg[i])
      return m_ErrMsg[i];
    else
      return (m_Msg) ? m_Msg : "Unknown error";

  } // end of GetErrorMessage

/***********************************************************************/
/*  ODBConn construction/destruction.                                  */
/***********************************************************************/
ODBConn::ODBConn(PGLOBAL g, TDBODBC *tdbp)
  {
  m_G = g;
  m_Tdb = tdbp;
  m_henv = SQL_NULL_HENV;
  m_hdbc = SQL_NULL_HDBC;
//m_Recset = NULL
  m_hstmt = SQL_NULL_HSTMT;
  m_LoginTimeout = DEFAULT_LOGIN_TIMEOUT;
  m_QueryTimeout = DEFAULT_QUERY_TIMEOUT;
  m_UpdateOptions = 0;
  m_RowsetSize = (DWORD)((tdbp) ? tdbp->Rows : 10);
  m_Catver = (tdbp) ? tdbp->Catver : 0;
  m_Rows = 0;
  m_Fetch = 0;
	m_Fp = NULL;
  m_Connect = NULL;
  m_User = NULL;
  m_Pwd = NULL;
  m_Updatable = true;
  m_Transact = false;
  m_Scrollable = (tdbp) ? tdbp->Scrollable : false;
  m_Full = false;
  m_UseCnc = false;
  m_IDQuoteChar[0] = '"';
  m_IDQuoteChar[1] = 0;
//*m_ErrMsg = '\0';
  } // end of ODBConn

//ODBConn::~ODBConn()
//  {
//if (Connected())
//  EndCom();

//  } // end of ~ODBConn

/***********************************************************************/
/*  Screen for errors.                                                 */
/***********************************************************************/
bool ODBConn::Check(RETCODE rc)
  {
  switch (rc) {
    case SQL_SUCCESS_WITH_INFO:
      if (trace(1)) {
        DBX x(rc);

        if (x.BuildErrorMessage(this, m_hstmt))
          htrc("ODBC Success With Info, hstmt=%p %s\n",
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
/*  DB exception throw routines.                                       */
/***********************************************************************/
void ODBConn::ThrowDBX(RETCODE rc, PCSZ msg, HSTMT hstmt)
  {
  DBX* xp = new(m_G) DBX(rc, msg);

  // Don't throw if no error
  if (xp->BuildErrorMessage(this, hstmt))
    throw xp;

  } // end of ThrowDBX

void ODBConn::ThrowDBX(PCSZ msg)
  {
  DBX* xp = new(m_G) DBX(0, "Error");

  xp->m_ErrMsg[0] = msg;
  throw xp;
  } // end of ThrowDBX

/***********************************************************************/
/*  Utility routine.                                                   */
/***********************************************************************/
PSZ ODBConn::GetStringInfo(ushort infotype)
  {
//ASSERT(m_hdbc != SQL_NULL_HDBC);
  char   *p, buffer[MAX_STRING_INFO];
  SWORD   result;
  RETCODE rc;

  rc = SQLGetInfo(m_hdbc, infotype, buffer, sizeof(buffer), &result);

  if (!Check(rc)) {
    ThrowDBX(rc, "SQLGetInfo");  // Temporary
//  *buffer = '\0';
    } // endif rc

  p = PlugDup(m_G, buffer);
  return p;
  } // end of GetStringInfo

/***********************************************************************/
/*  Utility routine.                                                   */
/***********************************************************************/
int ODBConn::GetMaxValue(ushort infotype)
  {
//ASSERT(m_hdbc != SQL_NULL_HDBC);
  ushort  maxval;
  RETCODE rc;

  rc = SQLGetInfo(m_hdbc, infotype, &maxval, 0, NULL);

  if (!Check(rc))
    maxval = 0;

  return (int)maxval;
  } // end of GetMaxValue

/***********************************************************************/
/*  Utility routines.                                                  */
/***********************************************************************/
void ODBConn::OnSetOptions(HSTMT hstmt)
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

/***********************************************************************/
/*  Open: connect to a data source.                                    */
/***********************************************************************/
int ODBConn::Open(PCSZ ConnectString, POPARM sop, DWORD options)
  {
  PGLOBAL& g = m_G;
//ASSERT_VALID(this);
//ASSERT(ConnectString == NULL || AfxIsValidString(ConnectString));
  ASSERT(!(options & noOdbcDialog && options & forceOdbcDialog));

  m_Updatable = !(options & openReadOnly);
  m_Connect = ConnectString;
  m_User = sop->User;
  m_Pwd = sop->Pwd;
  m_LoginTimeout = sop->Cto;
  m_QueryTimeout = sop->Qto;
  m_UseCnc = sop->UseCnc;

  // Allocate the HDBC and make connection
  try {
    /*PSZ ver;*/

    AllocConnect(options);
    /*ver = GetStringInfo(SQL_ODBC_VER);*/

    if (!m_UseCnc) {
      if (DriverConnect(options)) {
        strcpy(g->Message, MSG(CONNECT_CANCEL));
        return 0;
        } // endif

    } else           // Connect using SQLConnect
      Connect();

		/*********************************************************************/
		/*  Link a Fblock. This make possible to automatically close it      */
		/*  in case of error (throw).                                        */
		/*********************************************************************/
		PDBUSER dbuserp = (PDBUSER)g->Activityp->Aptr;

		m_Fp = (PFBLOCK)PlugSubAlloc(g, NULL, sizeof(FBLOCK));
		m_Fp->Type = TYPE_FB_ODBC;
		m_Fp->Fname = NULL;
		m_Fp->Next = dbuserp->Openlist;
		dbuserp->Openlist = m_Fp;
		m_Fp->Count = 1;
		m_Fp->Length = 0;
		m_Fp->Memory = NULL;
		m_Fp->Mode = MODE_ANY;
		m_Fp->File = this;
		m_Fp->Handle = 0;

		/*ver = GetStringInfo(SQL_DRIVER_ODBC_VER);*/
    // Verify support for required functionality and cache info
//  VerifyConnect();         Deprecated
    GetConnectInfo();
  } catch(DBX *xp) {
    sprintf(g->Message, "%s: %s", xp->m_Msg, xp->GetErrorMessage(0));
    Close();
//  Free();
    return -1;
  } // end try-catch

  return 1;
  } // end of Open

/***********************************************************************/
/*  Allocate an henv (first time called) and hdbc.                     */
/***********************************************************************/
void ODBConn::AllocConnect(DWORD Options)
  {
  if (m_hdbc != SQL_NULL_HDBC)
    return;

  RETCODE rc;
//AfxLockGlobals(CRIT_ODBC);

  // Need to allocate an environment for first connection
  if (m_henv == SQL_NULL_HENV) {
//  ASSERT(m_nAlloc == 0);

    rc = SQLAllocEnv(&m_henv);

    if (!Check(rc)) {
//    AfxUnlockGlobals(CRIT_ODBC);
      ThrowDBX(rc, "SQLAllocEnv");  // Fatal
      } // endif rc

    } // endif m_henv

  // Do the real thing, allocating connection data
  rc = SQLAllocConnect(m_henv, &m_hdbc);

  if (!Check(rc)) {
//  AfxUnlockGlobals(CRIT_ODBC);
    ThrowDBX(rc, "SQLAllocConnect");  // Fatal
    } // endif rc

//m_nAlloc++;                          // allocated at last
//AfxUnlockGlobals(CRIT_ODBC);

#if defined(_DEBUG)
  if (Options & traceSQL) {
    SQLSetConnectOption(m_hdbc, SQL_OPT_TRACEFILE, (SQLULEN)"xodbc.out");
    SQLSetConnectOption(m_hdbc, SQL_OPT_TRACE, 1);
    } // endif
#endif // _DEBUG

  if ((signed)m_LoginTimeout >= 0) {
    rc = SQLSetConnectOption(m_hdbc, SQL_LOGIN_TIMEOUT, m_LoginTimeout);

    if (trace(1) && rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO)
      htrc("Warning: Failure setting login timeout\n");

    } // endif Timeout

  if (!m_Updatable) {
    rc = SQLSetConnectOption(m_hdbc, SQL_ACCESS_MODE, SQL_MODE_READ_ONLY);
    
    if (trace(1) && rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO)
      htrc("Warning: Failure setting read only access mode\n");

    } // endif

  // Turn on cursor lib support
  if (Options & useCursorLib)
    rc = SQLSetConnectOption(m_hdbc, SQL_ODBC_CURSORS, SQL_CUR_USE_DRIVER);

  return;
  } // end of AllocConnect

/***********************************************************************/
/*  Connect to data source using SQLConnect.                           */
/***********************************************************************/
void ODBConn::Connect(void)
  {
  SQLRETURN   rc;
  SQLSMALLINT ul = (m_User ? SQL_NTS : 0); 
  SQLSMALLINT pl = (m_Pwd ? SQL_NTS : 0);

  rc = SQLConnect(m_hdbc, (SQLCHAR*)m_Connect, SQL_NTS, 
                          (SQLCHAR*)m_User, ul, (SQLCHAR*)m_Pwd, pl);
                  
  if (!Check(rc))
    ThrowDBX(rc, "SQLConnect");

  } // end of Connect

/***********************************************************************/
/*  Connect to data source using SQLDriverConnect.                     */
/***********************************************************************/
bool ODBConn::DriverConnect(DWORD Options)
  {
  RETCODE rc;
  SWORD   nResult;
  PUCHAR  ConnOut = (PUCHAR)PlugSubAlloc(m_G, NULL, MAX_CONNECT_LEN);
  UWORD   wConnectOption = SQL_DRIVER_COMPLETE;
#if defined(_WIN32)
  HWND    hWndTop = GetForegroundWindow();
  HWND    hWnd = GetParent(hWndTop);

  if (hWnd == NULL)
    hWnd = GetDesktopWindow();
#else   // !_WIN32
  HWND    hWnd = (HWND)1;
#endif  // !_WIN32
  PGLOBAL& g = m_G;
  PDBUSER dup = PlgGetUser(g);

//if (Options & noOdbcDialog || dup->Remote)
    wConnectOption = SQL_DRIVER_NOPROMPT;
//else if (Options & forceOdbcDialog)
//  wConnectOption = SQL_DRIVER_PROMPT;

  rc = SQLDriverConnect(m_hdbc, hWnd, (PUCHAR)m_Connect,
                        SQL_NTS, ConnOut, MAX_CONNECT_LEN,
                        &nResult, wConnectOption);

#if defined(_WIN32)
  if (hWndTop)
    EnableWindow(hWndTop, true);
#endif   // _WIN32

  // If user hit 'Cancel'
  if (rc == SQL_NO_DATA_FOUND) {
    Close();
//  Free();
    return true;
    } // endif rc

  if (!Check(rc))
    ThrowDBX(rc, "SQLDriverConnect");

  // Save connect string returned from ODBC
  m_Connect = (PSZ)ConnOut;

  // All done
  return false;
  } // end of DriverConnect

void ODBConn::VerifyConnect()
  {
#if defined(NEWMSG) || defined(XMSG)
  PGLOBAL& g = m_G;
#endif   // NEWMSG  ||         XMSG
  RETCODE  rc;
  SWORD    result;
  SWORD    conformance;

  rc = SQLGetInfo(m_hdbc, SQL_ODBC_API_CONFORMANCE,
                  &conformance, sizeof(conformance), &result);

  if (!Check(rc))
    ThrowDBX(rc, "SQLGetInfo");

  if (conformance < SQL_OAC_LEVEL1)
    ThrowDBX(MSG(API_CONF_ERROR));

  rc = SQLGetInfo(m_hdbc, SQL_ODBC_SQL_CONFORMANCE,
                  &conformance, sizeof(conformance), &result);

  if (!Check(rc))
    ThrowDBX(rc, "SQLGetInfo");

  if (conformance < SQL_OSC_MINIMUM)
    ThrowDBX(MSG(SQL_CONF_ERROR));

  } // end of VerifyConnect

void ODBConn::GetConnectInfo()
  {
  RETCODE rc;
  SWORD   nResult;
#if 0                   // Update not implemented yet
  UDWORD  DrvPosOp;

  // Reset the database update options
  m_UpdateOptions = 0;

  // Check for SQLSetPos support
  rc = SQLGetInfo(m_hdbc, SQL_POS_OPERATIONS,
                  &DrvPosOp, sizeof(DrvPosOp), &nResult);

  if (Check(rc) &&
      (DrvPosOp & SQL_POS_UPDATE) &&
      (DrvPosOp & SQL_POS_DELETE) &&
      (DrvPosOp & SQL_POS_ADD))
     m_UpdateOptions = SQL_SETPOSUPDATES;

  // Check for positioned update SQL support
  UDWORD PosStatements;

  rc = SQLGetInfo(m_hdbc, SQL_POSITIONED_STATEMENTS,
                        &PosStatements, sizeof(PosStatements),
                        &nResult);

  if (Check(rc) &&
      (PosStatements & SQL_PS_POSITIONED_DELETE) &&
      (PosStatements & SQL_PS_POSITIONED_UPDATE))
    m_UpdateOptions |= SQL_POSITIONEDSQL;

  if (m_Updatable) {
    // Make sure data source is Updatable
    char ReadOnly[10];

    rc = SQLGetInfo(m_hdbc, SQL_DATA_SOURCE_READ_ONLY,
                    ReadOnly, sizeof(ReadOnly), &nResult);

    if (Check(rc) && nResult == 1)
      m_Updatable = !!strcmp(ReadOnly, "Y");
    else
      m_Updatable = false;

    if (trace(1))
      htrc("Warning: data source is readonly\n");

  } else // Make data source is !Updatable
    rc = SQLSetConnectOption(m_hdbc, SQL_ACCESS_MODE,
                                     SQL_MODE_READ_ONLY);
#endif   // 0

  // Get the quote char to use when constructing SQL
  rc = SQLGetInfo(m_hdbc, SQL_IDENTIFIER_QUOTE_CHAR,
                  m_IDQuoteChar, sizeof(m_IDQuoteChar), &nResult);

  if (trace(1))
    htrc("DBMS: %s, Version: %s, rc=%d\n",
         GetStringInfo(SQL_DBMS_NAME), GetStringInfo(SQL_DBMS_VER), rc);

  } // end of GetConnectInfo

/***********************************************************************/
/*  Allocate record set and execute an SQL query.                      */
/***********************************************************************/
int ODBConn::ExecDirectSQL(char *sql, ODBCCOL *tocols)
  {
  PGLOBAL& g = m_G;
  void    *buffer;
  bool     b;
  UWORD    n, k;
  SWORD    len, tp, ncol = 0;
  ODBCCOL *colp;
  RETCODE  rc;
  HSTMT    hstmt;

  try {
    b = false;

    if (m_hstmt) {
      // This is a Requery
      rc = SQLFreeStmt(m_hstmt, SQL_CLOSE);

      if (!Check(rc))
        ThrowDBX(rc, "SQLFreeStmt", m_hstmt);

      m_hstmt = NULL;
      } // endif m_hstmt

    rc = SQLAllocStmt(m_hdbc, &hstmt);

    if (!Check(rc))
      ThrowDBX(rc, "SQLAllocStmt");

    if (m_Scrollable) {
      rc = SQLSetStmtAttr(hstmt, SQL_ATTR_CURSOR_SCROLLABLE, 
                          (void*)SQL_SCROLLABLE, 0);

      if (!Check(rc))
        ThrowDBX(rc, "Scrollable", hstmt);

      } // endif m_Scrollable

    OnSetOptions(hstmt);
    b = true;

    if (trace(1))
      htrc("ExecDirect hstmt=%p %.256s\n", hstmt, sql);

    if (m_Tdb->Srcdef) {
      // Be sure this is a query returning a result set
      do {
        rc = SQLPrepare(hstmt, (PUCHAR)sql, SQL_NTS);
        } while (rc == SQL_STILL_EXECUTING);

      if (!Check(rc))
        ThrowDBX(rc, "SQLPrepare", hstmt);

      if (!Check(rc = SQLNumResultCols(hstmt, &ncol)))
        ThrowDBX(rc, "SQLNumResultCols", hstmt);

      if (ncol == 0) {
        strcpy(g->Message, "This Srcdef does not return a result set");
        return -1;
        } // endif ncol

      // Ok, now we can proceed
      do {
        rc = SQLExecute(hstmt);
        } while (rc == SQL_STILL_EXECUTING);

      if (!Check(rc))
        ThrowDBX(rc, "SQLExecute", hstmt);

    } else {
      do {
        rc = SQLExecDirect(hstmt, (PUCHAR)sql, SQL_NTS);
      } while (rc == SQL_STILL_EXECUTING);

      if (!Check(rc))
        ThrowDBX(rc, "SQLExecDirect", hstmt);

      do {
        rc = SQLNumResultCols(hstmt, &ncol);
      } while (rc == SQL_STILL_EXECUTING);

      k = 0;    // used for column number
    } // endif Srcdef

    for (n = 0, colp = tocols; colp; colp = (PODBCCOL)colp->GetNext())
      if (!colp->IsSpecial())
        n++;

    // n can be 0 for query such as Select count(*) from table
    if (n && n > (UWORD)ncol)
      ThrowDBX(MSG(COL_NUM_MISM));

    // Now bind the column buffers
    for (colp = tocols; colp; colp = (PODBCCOL)colp->GetNext())
      if (!colp->IsSpecial()) {
        buffer = colp->GetBuffer(m_RowsetSize);
        len = colp->GetBuflen();
        tp = GetSQLCType(colp->GetResultType());

        if (tp == SQL_TYPE_NULL) {
          sprintf(m_G->Message, MSG(INV_COLUMN_TYPE),
                  colp->GetResultType(), SVP(colp->GetName()));
          ThrowDBX(m_G->Message);
        } // endif tp

        if (m_Tdb->Srcdef)
          k = colp->GetIndex();
        else
          k++;

        if (trace(1))
          htrc("Binding col=%u type=%d buf=%p len=%d slen=%p\n",
                  k, tp, buffer, len, colp->GetStrLen());

        rc = SQLBindCol(hstmt, k, tp, buffer, len, colp->GetStrLen());

        if (!Check(rc))
          ThrowDBX(rc, "SQLBindCol", hstmt);

      } // endif colp

  } catch(DBX *x) {
    if (trace(1))
      for (int i = 0; i < MAX_NUM_OF_MSG && x->m_ErrMsg[i]; i++)
        htrc(x->m_ErrMsg[i]);

    sprintf(m_G->Message, "%s: %s", x->m_Msg, x->GetErrorMessage(0));

    if (b)
      SQLCancel(hstmt);

    rc = SQLFreeStmt(hstmt, SQL_DROP);
    m_hstmt = NULL;
    return -1;
  } // end try/catch

  m_hstmt = hstmt;
  return (int)m_RowsetSize;   // May have been reset in OnSetOptions
  } // end of ExecDirectSQL

/***********************************************************************/
/*  Get the number of lines of the result set.                         */
/***********************************************************************/
int ODBConn::GetResultSize(char *sql, ODBCCOL *colp)
  {
  int    n = 0;
  RETCODE rc;

  if (ExecDirectSQL(sql, colp) < 0)
    return -1;

  try {
    for (n = 0; ; n++) {
      do {
        rc = SQLFetch(m_hstmt);
        } while (rc == SQL_STILL_EXECUTING);

      if (!Check(rc))
        ThrowDBX(rc, "SQLFetch", m_hstmt);

      if (rc == SQL_NO_DATA_FOUND)
        break;

      } // endfor n

  } catch(DBX *x) {
    strcpy(m_G->Message, x->GetErrorMessage(0));
    
    if (trace(1))
      for (int i = 0; i < MAX_NUM_OF_MSG && x->m_ErrMsg[i]; i++)
        htrc(x->m_ErrMsg[i]);

    SQLCancel(m_hstmt);
    n = -2;
  } // end try/catch

  rc = SQLFreeStmt(m_hstmt, SQL_DROP);
  m_hstmt = NULL;

  if (n != 1)
    return -3;
  else
    return colp->GetIntValue();

  } // end of GetResultSize

/***********************************************************************/
/*  Fetch next row.                                                    */
/***********************************************************************/
int ODBConn::Fetch(int pos)
  {
  ASSERT(m_hstmt);
  int      irc;
  SQLULEN  crow;
  RETCODE  rc;
  PGLOBAL& g = m_G;

  try {
//  do {
    if (pos) {
      rc = SQLExtendedFetch(m_hstmt, SQL_FETCH_ABSOLUTE, pos, &crow, NULL);
    } else if (m_RowsetSize) {
      rc = SQLExtendedFetch(m_hstmt, SQL_FETCH_NEXT, 1, &crow, NULL);
    } else {
      rc = SQLFetch(m_hstmt);
      crow = 1;
    } // endif m_RowsetSize
//    } while (rc == SQL_STILL_EXECUTING);

    if (trace(2))
      htrc("Fetch: hstmt=%p RowseSize=%d rc=%d\n",
                     m_hstmt, m_RowsetSize, rc);

    if (!Check(rc))
      ThrowDBX(rc, "Fetching", m_hstmt);

    if (rc == SQL_NO_DATA_FOUND) {
      m_Full = (m_Fetch == 1);
      irc = 0;
    } else
      irc = (int)crow;

    m_Fetch++;
    m_Rows += irc;
  } catch(DBX *x) {
    if (trace(1))
      for (int i = 0; i < MAX_NUM_OF_MSG && x->m_ErrMsg[i]; i++)
        htrc(x->m_ErrMsg[i]);

    sprintf(g->Message, "%s: %s", x->m_Msg, x->GetErrorMessage(0));
    irc = -1;
  } // end try/catch

  return irc;
  } // end of Fetch

/***********************************************************************/
/*  Prepare an SQL statement for insert.                               */
/***********************************************************************/
int ODBConn::PrepareSQL(char *sql)
  {
  PGLOBAL& g = m_G;
  bool     b;
  UINT     txn = 0;
  SWORD    nparm;
  RETCODE  rc;
  HSTMT    hstmt;

  if (m_Tdb->GetMode() != MODE_READ) {
    // Does the data source support transactions
    rc = SQLGetInfo(m_hdbc, SQL_TXN_CAPABLE, &txn, 0, NULL);

    if (Check(rc) && txn != SQL_TC_NONE) try {
      rc = SQLSetConnectAttr(m_hdbc, SQL_ATTR_AUTOCOMMIT,
                                     SQL_AUTOCOMMIT_OFF, SQL_IS_UINTEGER);
      
      if (!Check(rc))
        ThrowDBX(SQL_INVALID_HANDLE, "SQLSetConnectAttr");

      m_Transact = true;
    } catch(DBX *x) {
      if (trace(1))
        for (int i = 0; i < MAX_NUM_OF_MSG && x->m_ErrMsg[i]; i++)
          htrc(x->m_ErrMsg[i]);

    sprintf(g->Message, "%s: %s", x->m_Msg, x->GetErrorMessage(0));
    } // end try/catch

    } // endif Mode

  try {
    b = false;

    if (m_hstmt) {
      RETCODE rc = SQLFreeStmt(m_hstmt, SQL_CLOSE);

      hstmt = m_hstmt;
      m_hstmt = NULL;

      if (m_Tdb->GetAmType() != TYPE_AM_XDBC)
        ThrowDBX(MSG(SEQUENCE_ERROR));

      } // endif m_hstmt

    rc = SQLAllocStmt(m_hdbc, &hstmt);

    if (!Check(rc))
      ThrowDBX(SQL_INVALID_HANDLE, "SQLAllocStmt");

    OnSetOptions(hstmt);
    b = true;

    if (trace(1))
      htrc("Prepare hstmt=%p %.64s\n", hstmt, sql);

    do {
      rc = SQLPrepare(hstmt, (PUCHAR)sql, SQL_NTS);
      } while (rc == SQL_STILL_EXECUTING);

    if (!Check(rc))
      ThrowDBX(rc, "SQLPrepare", hstmt);

    do {
      rc = SQLNumParams(hstmt, &nparm);
      } while (rc == SQL_STILL_EXECUTING);

  } catch(DBX *x) {
    if (trace(1))
      for (int i = 0; i < MAX_NUM_OF_MSG && x->m_ErrMsg[i]; i++)
        htrc(x->m_ErrMsg[i]);

    sprintf(g->Message, "%s: %s", x->m_Msg, x->GetErrorMessage(0));

    if (b)
      SQLCancel(hstmt);

    rc = SQLFreeStmt(hstmt, SQL_DROP);
    m_hstmt = NULL;

    if (m_Transact) {
      rc = SQLEndTran(SQL_HANDLE_DBC, m_hdbc, SQL_ROLLBACK);
      m_Transact = false;
      } // endif m_Transact

    return -1;
  } // end try/catch

  m_hstmt = hstmt;
  return (int)nparm;
  } // end of PrepareSQL

/***********************************************************************/
/*  Execute a prepared statement.                                      */
/***********************************************************************/
int ODBConn::ExecuteSQL(void)
  {
  PGLOBAL& g = m_G;
  SWORD    ncol = 0;
  RETCODE  rc;
  SQLLEN   afrw = -1;

  try {
    do {
      rc = SQLExecute(m_hstmt);
      } while (rc == SQL_STILL_EXECUTING);

    if (!Check(rc))
      ThrowDBX(rc, "SQLExecute", m_hstmt);

    if (!Check(rc = SQLNumResultCols(m_hstmt, &ncol)))
      ThrowDBX(rc, "SQLNumResultCols", m_hstmt);

    if (ncol) {
      // This should never happen while inserting
      strcpy(g->Message, "Logical error while inserting");
    } else {
      // Insert, Update or Delete statement
      if (!Check(rc = SQLRowCount(m_hstmt, &afrw)))
        ThrowDBX(rc, "SQLRowCount", m_hstmt);

    } // endif ncol

  } catch(DBX *x) {
    sprintf(m_G->Message, "%s: %s", x->m_Msg, x->GetErrorMessage(0));
    SQLCancel(m_hstmt);
    rc = SQLFreeStmt(m_hstmt, SQL_DROP);
    m_hstmt = NULL;

    if (m_Transact) {
      rc = SQLEndTran(SQL_HANDLE_DBC, m_hdbc, SQL_ROLLBACK);
      m_Transact = false;
      } // endif m_Transact

    afrw = -1;
  } // end try/catch

  return (int)afrw;
  } // end of ExecuteSQL

/***********************************************************************/
/*  Bind a parameter for inserting.                                    */
/***********************************************************************/
bool ODBConn::BindParam(ODBCCOL *colp)
  {
  void        *buf;
  int          buftype = colp->GetResultType();
  SQLUSMALLINT n = colp->GetRank();
	SQLSMALLINT  ct, sqlt, dec, nul __attribute__((unused));
  SQLULEN      colsize;
  SQLLEN       len;
  SQLLEN      *strlen = colp->GetStrLen();
  SQLRETURN    rc;

#if 0
  try {
		// This function is often not or badly implemented by data sources
    rc = SQLDescribeParam(m_hstmt, n, &sqlt, &colsize, &dec, &nul);

    if (!Check(rc))
      ThrowDBX(rc, "SQLDescribeParam", m_hstmt);

  } catch(DBX *x) {
    sprintf(m_G->Message, "%s: %s", x->m_Msg, x->GetErrorMessage(0));
#endif // 0
    colsize = colp->GetPrecision();
    sqlt = GetSQLType(buftype);
		dec = IsTypeNum(buftype) ? colp->GetScale() : 0;
		nul = colp->IsNullable() ? SQL_NULLABLE : SQL_NO_NULLS;
//} // end try/catch

  buf = colp->GetBuffer(0);
  len = IsTypeChar(buftype) ? colp->GetBuflen() : 0;
  ct = GetSQLCType(buftype);
  *strlen = IsTypeChar(buftype) ? SQL_NTS : 0;

  try {
    rc = SQLBindParameter(m_hstmt, n, SQL_PARAM_INPUT, ct, sqlt,
                          colsize, dec, buf, len, strlen);

    if (!Check(rc))
      ThrowDBX(rc, "SQLBindParameter", m_hstmt);

  } catch(DBX *x) {
    strcpy(m_G->Message, x->GetErrorMessage(0));
    SQLCancel(m_hstmt);
    rc = SQLFreeStmt(m_hstmt, SQL_DROP);
    m_hstmt = NULL;
    return true;
  } // end try/catch

  return false;
  } // end of BindParam

/***********************************************************************/
/*  Execute an SQL command.                                            */
/***********************************************************************/
bool ODBConn::ExecSQLcommand(char *sql)
  {
  char     cmd[16];
  bool     b, rcd = false;
  UINT     txn = 0;
  PGLOBAL& g = m_G;
  SWORD    ncol = 0;
  SQLLEN   afrw;
  RETCODE  rc;
  HSTMT    hstmt;

  try {
    b = FALSE;

    // Check whether we should use transaction
    if (sscanf(sql, " %15s ", cmd) == 1) {
      if (!stricmp(cmd, "INSERT") || !stricmp(cmd, "UPDATE") ||
          !stricmp(cmd, "DELETE") || !stricmp(cmd, "REPLACE")) {
        // Does the data source support transactions
        rc = SQLGetInfo(m_hdbc, SQL_TXN_CAPABLE, &txn, 0, NULL);
    
        if (Check(rc) && txn != SQL_TC_NONE) {
          rc = SQLSetConnectAttr(m_hdbc, SQL_ATTR_AUTOCOMMIT,
                                 SQL_AUTOCOMMIT_OFF, SQL_IS_UINTEGER);
        
          if (!Check(rc))
            ThrowDBX(SQL_INVALID_HANDLE, "SQLSetConnectAttr");
    
          m_Transact = TRUE;
          } // endif txn

        } // endif cmd

      } // endif sql

    // Allocate the statement handle
    rc = SQLAllocStmt(m_hdbc, &hstmt);

    if (!Check(rc))
      ThrowDBX(SQL_INVALID_HANDLE, "SQLAllocStmt");

    OnSetOptions(hstmt);
    b = true;

    if (trace(1))
      htrc("ExecSQLcommand hstmt=%p %.64s\n", hstmt, sql);

    // Proceed with command execution
    do {
      rc = SQLExecDirect(hstmt, (PUCHAR)sql, SQL_NTS);
      } while (rc == SQL_STILL_EXECUTING);

    if (!Check(rc))
      ThrowDBX(rc, "SQLExecDirect", hstmt);

    // Check whether this is a query returning a result set
    if (!Check(rc = SQLNumResultCols(hstmt, &ncol)))
      ThrowDBX(rc, "SQLNumResultCols", hstmt);

    if (!ncol) {
      if (!Check(SQLRowCount(hstmt, &afrw)))
        ThrowDBX(rc, "SQLRowCount", hstmt);

      m_Tdb->AftRows = (int)afrw;
      strcpy(g->Message, "Affected rows");
    } else {
      m_Tdb->AftRows = (int)ncol;
      strcpy(g->Message, "Result set column number");
    } // endif ncol

  } catch(DBX *x) {
		if (trace(1))
			for (int i = 0; i < MAX_NUM_OF_MSG && x->m_ErrMsg[i]; i++)
				htrc(x->m_ErrMsg[i]);

    sprintf(g->Message, "Remote %s: %s", x->m_Msg, x->GetErrorMessage(0));

    if (b)
      SQLCancel(hstmt);

    m_Tdb->AftRows = -1;
    rcd = true;
  } // end try/catch

  if (!Check(rc = SQLFreeStmt(hstmt, SQL_CLOSE)))
    sprintf(g->Message, "SQLFreeStmt: rc=%d", rc);

  if (m_Transact) {
    // Terminate the transaction
    if (!Check(rc = SQLEndTran(SQL_HANDLE_DBC, m_hdbc, 
                       (rcd) ? SQL_ROLLBACK : SQL_COMMIT)))
      sprintf(g->Message, "SQLEndTran: rc=%d", rc);

    if (!Check(rc = SQLSetConnectAttr(m_hdbc, SQL_ATTR_AUTOCOMMIT,
               (SQLPOINTER)SQL_AUTOCOMMIT_ON, SQL_IS_UINTEGER)))
      sprintf(g->Message, "SQLSetConnectAttr: rc=%d", rc);

    m_Transact = false;
    } // endif m_Transact

  return rcd;
  } // end of ExecSQLcommand

/**************************************************************************/
/*  GetMetaData: constructs the result blocks containing the              */
/*  description of all the columns of an SQL command.                     */
/**************************************************************************/
PQRYRES ODBConn::GetMetaData(PGLOBAL g, PCSZ dsn, PCSZ src)
  {
  static int  buftyp[] = {TYPE_STRING, TYPE_SHORT, TYPE_INT,
                          TYPE_SHORT,  TYPE_SHORT};
  static XFLD fldtyp[] = {FLD_NAME,  FLD_TYPE, FLD_PREC,
                          FLD_SCALE, FLD_NULL};
  static unsigned int length[] = {0, 6, 10, 6, 6};
  unsigned char cn[60];
  int     qcol = 5;
  short   nl, type, prec, nul, cns = (short)sizeof(cn);
  PQRYRES qrp = NULL;
  PCOLRES crp;
  USHORT  i;
  SQLULEN n;
  SWORD   ncol;
  RETCODE rc;
  HSTMT   hstmt;

  try {
    rc = SQLAllocStmt(m_hdbc, &hstmt);

    if (!Check(rc))
      ThrowDBX(SQL_INVALID_HANDLE, "SQLAllocStmt");

    OnSetOptions(hstmt);

    do {
      rc = SQLPrepare(hstmt, (PUCHAR)src, SQL_NTS);
//    rc = SQLExecDirect(hstmt, (PUCHAR)src, SQL_NTS);
      } while (rc == SQL_STILL_EXECUTING);

    if (!Check(rc))
      ThrowDBX(rc, "SQLExecDirect", hstmt);

    do {
      rc = SQLNumResultCols(hstmt, &ncol);
      } while (rc == SQL_STILL_EXECUTING);

    if (!Check(rc))
      ThrowDBX(rc, "SQLNumResultCols", hstmt);

    if (ncol) for (i = 1; i <= ncol; i++) {
      do {
        rc = SQLDescribeCol(hstmt, i, NULL, 0, &nl, NULL, NULL, NULL, NULL); 
        } while (rc == SQL_STILL_EXECUTING);

      if (!Check(rc))
        ThrowDBX(rc, "SQLDescribeCol", hstmt);

      length[0] = MY_MAX(length[0], (UINT)nl);
      } // endfor i

  } catch(DBX *x) {
    sprintf(g->Message, "%s: %s", x->m_Msg, x->GetErrorMessage(0));
    goto err;
  } // end try/catch

  if (!ncol) {
    strcpy(g->Message, "Invalid Srcdef");
    goto err;
    } // endif ncol

  /************************************************************************/
  /*  Allocate the structures used to refer to the result set.            */
  /************************************************************************/
  if (!(qrp = PlgAllocResult(g, qcol, ncol, IDS_COLUMNS + 3,
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
  try {
    for (i = 0; i < ncol; i++) {
      do {
        rc = SQLDescribeCol(hstmt, i+1, cn, cns, &nl, &type, &n, &prec, &nul); 
        } while (rc == SQL_STILL_EXECUTING);

      if (!Check(rc))
        ThrowDBX(rc, "SQLDescribeCol", hstmt);
      else
        qrp->Nblin++;

      crp = qrp->Colresp;                    // Column_Name
      crp->Kdata->SetValue((char*)cn, i);
      crp = crp->Next;                       // Data_Type
      crp->Kdata->SetValue(type, i);
      crp = crp->Next;                       // Precision (length)
      crp->Kdata->SetValue((int)n, i);
      crp = crp->Next;                       // Scale
      crp->Kdata->SetValue(prec, i);
      crp = crp->Next;                       // Nullable
      crp->Kdata->SetValue(nul, i);
      } // endfor i

  } catch(DBX *x) {
    sprintf(g->Message, "%s: %s", x->m_Msg, x->GetErrorMessage(0));
    qrp = NULL;
  } // end try/catch

  /* Cleanup */
 err:
  SQLCancel(hstmt);
  rc = SQLFreeStmt(hstmt, SQL_DROP);
  Close();

  /************************************************************************/
  /*  Return the result pointer for use by GetData routines.              */
  /************************************************************************/
  return qrp;
  } // end of GetMetaData

/***********************************************************************/
/*  Get the list of Data Sources and set it in qrp.                    */
/***********************************************************************/
bool ODBConn::GetDataSources(PQRYRES qrp)
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
      ThrowDBX(rc, "SQLAllocEnv"); // Fatal

    for (int i = 0; i < qrp->Maxres; i++) {
      dsn = (UCHAR*)crp1->Kdata->GetValPtr(i);
      des = (UCHAR*)crp2->Kdata->GetValPtr(i);
      rc = SQLDataSources(m_henv, dir, dsn, n1, &p1, des, n2, &p2);

      if (rc == SQL_NO_DATA_FOUND)
        break;
      else if (!Check(rc))
        ThrowDBX(rc, "SQLDataSources");

      qrp->Nblin++;
      dir = SQL_FETCH_NEXT;
      } // endfor i

  } catch(DBX *x) {
    sprintf(m_G->Message, "%s: %s", x->m_Msg, x->GetErrorMessage(0));
    rv = true;
  } // end try/catch

  Close();
  return rv;
  } // end of GetDataSources

/***********************************************************************/
/*  Get the list of Drivers and set it in qrp.                         */
/***********************************************************************/
bool ODBConn::GetDrivers(PQRYRES qrp)
  {
  int     i, n;
  bool    rv = false;
  UCHAR  *des, *att;
  UWORD   dir = SQL_FETCH_FIRST;
  SWORD   n1, n2, p1, p2;
  PCOLRES crp1 = qrp->Colresp, crp2 = qrp->Colresp->Next;
  RETCODE rc;

  n1 = crp1->Clen;
  n2 = crp2->Clen;

  try {
    rc = SQLAllocEnv(&m_henv);

    if (!Check(rc))
      ThrowDBX(rc, "SQLAllocEnv"); // Fatal

    for (n = 0; n < qrp->Maxres; n++) {
      des = (UCHAR*)crp1->Kdata->GetValPtr(n);
      att = (UCHAR*)crp2->Kdata->GetValPtr(n);
      rc = SQLDrivers(m_henv, dir, des, n1, &p1, att, n2, &p2);

      if (rc == SQL_NO_DATA_FOUND)
        break;
      else if (!Check(rc))
        ThrowDBX(rc, "SQLDrivers");


      // The attributes being separated by '\0', set them to ';'
      for (i = 0; i < p2; i++)
        if (!att[i])
          att[i] = ';';

      qrp->Nblin++;
      dir = SQL_FETCH_NEXT;
      } // endfor n

  } catch(DBX *x) {
    sprintf(m_G->Message, "%s: %s", x->m_Msg, x->GetErrorMessage(0));
    rv = true;
  } // end try/catch

  Close();
  return rv;
  } // end of GetDrivers

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
  SQLQualifiedName(CATPARM *cap)
  {
    const char *name = (const char *)cap->Tab;
    char       *db = (char *)cap->DB;
    size_t      len, i;

    // Initialize the parts
    for (i = 0 ; i < max_parts; i++)
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

  SQLCHAR *ptr(uint i)
  {
    DBUG_ASSERT(i < max_parts);
    return (SQLCHAR *) (m_part[i].length ? m_part[i].str : NULL);
  } // end of ptr

  SQLSMALLINT length(uint i)
  {
    DBUG_ASSERT(i < max_parts);
    return (SQLSMALLINT)m_part[i].length;
  } // end of length

}; // end of class SQLQualifiedName

/***********************************************************************/
/*  Allocate recset and call SQLTables, SQLColumns or SQLPrimaryKeys.  */
/***********************************************************************/
int ODBConn::GetCatInfo(CATPARM *cap)
  {
  PGLOBAL& g = m_G;
  void    *buffer;
  int      i, irc;
  bool     b;
  PCSZ     fnc = "Unknown";
  UWORD    n = 0;
  SWORD    ncol, len, tp;
  SQLULEN  crow = 0;
  PQRYRES  qrp = cap->Qrp;
  PCOLRES  crp;
  RETCODE  rc = 0;
  HSTMT    hstmt = NULL;
  SQLLEN  *vl, *vlen = NULL;
  PVAL    *pval = NULL;
  char*   *pbuf = NULL;

  try {
    b = false;

    if (!m_hstmt) {
      rc = SQLAllocStmt(m_hdbc, &hstmt);

      if (!Check(rc))
        ThrowDBX(SQL_INVALID_HANDLE, "SQLAllocStmt");

    } else
      ThrowDBX(MSG(SEQUENCE_ERROR));

    b = true;

    // Currently m_Catver should be always 0 here
    assert(!m_Catver);     // This may be temporary

    if (qrp->Maxres > 0)
      m_RowsetSize = 1;
    else
      ThrowDBX("0-sized result");

    SQLQualifiedName name(cap);

    // Now do call the proper ODBC API
    switch (cap->Id) {
      case CAT_TAB:
        fnc = "SQLTables";
        rc = SQLTables(hstmt, name.ptr(2), name.length(2),
                              name.ptr(1), name.length(1),
                              name.ptr(0), name.length(0),
					                    (SQLCHAR *)cap->Pat, 
					                    cap->Pat ? SQL_NTS : 0);
        break;
      case CAT_COL:
        fnc = "SQLColumns";
        rc = SQLColumns(hstmt, name.ptr(2), name.length(2),
                               name.ptr(1), name.length(1),
                               name.ptr(0), name.length(0),
															 (SQLCHAR *)cap->Pat, 
					                     cap->Pat ? SQL_NTS : 0);
        break;
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
        ThrowDBX("SQLSpecialColumns not available yet");
      default:
        ThrowDBX("Invalid SQL function id");
      } // endswitch infotype

    if (!Check(rc))
      ThrowDBX(rc, fnc, hstmt);

		// Some data source do not implement SQLNumResultCols
    if (Check(SQLNumResultCols(hstmt, &ncol)))
      // n because we no more ignore the first column
      if ((n = (UWORD)qrp->Nbcol) > (UWORD)ncol)
        ThrowDBX(MSG(COL_NUM_MISM));

    // Unconditional to handle STRBLK's
    pval = (PVAL *)PlugSubAlloc(g, NULL, n * sizeof(PVAL));
    vlen = (SQLLEN *)PlugSubAlloc(g, NULL, n * sizeof(SQLLEN));
    pbuf = (char**)PlugSubAlloc(g, NULL, n * sizeof(char*));

    // Now bind the column buffers
    for (n = 0, crp = qrp->Colresp; crp; crp = crp->Next) {
      if ((tp = GetSQLCType(crp->Type)) == SQL_TYPE_NULL) {
        sprintf(g->Message, MSG(INV_COLUMN_TYPE), crp->Type, crp->Name);
        ThrowDBX(g->Message);
        } // endif tp

      if (!(len = GetTypeSize(crp->Type, crp->Length))) {
        len = 255;           // for STRBLK's
        ((STRBLK*)crp->Kdata)->SetSorted(true);
        } // endif len

      pval[n] = AllocateValue(g, crp->Type, len);
			pval[n]->SetNullable(true);

      if (crp->Type == TYPE_STRING) {
        pbuf[n] = (char*)PlugSubAlloc(g, NULL, len);
        buffer = pbuf[n];
      } else
        buffer = pval[n]->GetTo_Val();

      vl = vlen + n;

      // n + 1 because column numbers begin with 1
      rc = SQLBindCol(hstmt, n + 1, tp, buffer, len, vl);

      if (!Check(rc))
        ThrowDBX(rc, "SQLBindCol", hstmt);

      n++;
      } // endfor crp

    fnc = "SQLFetch";

    // Now fetch the result
    // Extended fetch cannot be used because of STRBLK's
    for (i = 0; i < qrp->Maxres; i++) {
      if ((rc = SQLFetch(hstmt)) == SQL_NO_DATA_FOUND)
        break;
      else if (rc != SQL_SUCCESS) {
        if (trace(2) || (trace(1) && rc != SQL_SUCCESS_WITH_INFO)) {
          UCHAR   msg[SQL_MAX_MESSAGE_LENGTH + 1];
          UCHAR   state[SQL_SQLSTATE_SIZE + 1];
          RETCODE erc;
          SDWORD  native;

          htrc("SQLFetch: row %d rc=%d\n", i+1, rc);
          erc = SQLError(m_henv, m_hdbc, hstmt, state, &native, msg,
                         SQL_MAX_MESSAGE_LENGTH - 1, &len);

          if (rc != SQL_INVALID_HANDLE)
            // Skip non-errors
            for (n = 0; n < MAX_NUM_OF_MSG
                 && (erc == SQL_SUCCESS || erc == SQL_SUCCESS_WITH_INFO)
                 && strcmp((char*)state, "00000"); n++) {
              htrc("%s: %s, Native=%d\n", state, msg, native);
              erc = SQLError(m_henv, m_hdbc, hstmt, state, &native,
                             msg, SQL_MAX_MESSAGE_LENGTH - 1, &len);
              } // endfor n

          } // endif trace

        if (rc != SQL_SUCCESS_WITH_INFO)
          qrp->BadLines++;

        } // endif rc

      for (n = 0, crp = qrp->Colresp; crp; n++, crp = crp->Next) {
				if (vlen[n] == SQL_NO_TOTAL)
					ThrowDBX("Unexpected SQL_NO_TOTAL returned from SQLFetch");
				else if (vlen[n] == SQL_NULL_DATA)
          pval[n]->SetNull(true);
        else if (crp->Type == TYPE_STRING/* && vlen[n] != SQL_NULL_DATA*/)
          pval[n]->SetValue_char(pbuf[n], (int)vlen[n]);
        else
          pval[n]->SetNull(false);

        crp->Kdata->SetValue(pval[n], i);
        cap->Vlen[n][i] = vlen[n];
        } // endfor crp

      } // endfor i

#if 0
    if ((crow = i) && (rc == SQL_NO_DATA || rc == SQL_SUCCESS_WITH_INFO))
      rc = SQL_SUCCESS;

    if (rc == SQL_NO_DATA_FOUND) {
      if (cap->Pat)
        sprintf(g->Message, MSG(NO_TABCOL_DATA), cap->Tab, cap->Pat);
      else
        sprintf(g->Message, MSG(NO_TAB_DATA), cap->Tab);

      ThrowDBX(g->Message);
    } else if (rc == SQL_SUCCESS) {
      if ((rc = SQLFetch(hstmt)) != SQL_NO_DATA_FOUND)
        qrp->Truncated = true; 

    } else
      ThrowDBX(rc, fnc, hstmt);
#endif // 0

    if (!rc || rc == SQL_NO_DATA || rc == SQL_SUCCESS_WITH_INFO) {
      if ((rc = SQLFetch(hstmt)) != SQL_NO_DATA_FOUND)
        qrp->Truncated = true; 

      crow = i;
    } else
      ThrowDBX(rc, fnc, hstmt);

    irc = (int)crow;
  } catch(DBX *x) {
    if (trace(1))
      for (int i = 0; i < MAX_NUM_OF_MSG && x->m_ErrMsg[i]; i++)
        htrc(x->m_ErrMsg[i]);

    sprintf(g->Message, "%s: %s", x->m_Msg, x->GetErrorMessage(0));
    irc = -1;
  } // end try/catch

  if (b)
    SQLCancel(hstmt);

  // All this (hstmt vs> m_hstmt) to be revisited
  if (hstmt)
    rc = SQLFreeStmt(hstmt, SQL_DROP);

  return irc;
  } // end of GetCatInfo

/***********************************************************************/
/*  Allocate a CONNECT result structure from the ODBC result.          */
/***********************************************************************/
PQRYRES ODBConn::AllocateResult(PGLOBAL g)
  {
  bool         uns;
  PODBCCOL     colp;
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

  for (colp = (PODBCCOL)m_Tdb->Columns; colp; 
       colp = (PODBCCOL)colp->GetNext())
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
      crp->Clen = colp->GetBuflen();
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

/***********************************************************************/
/*  Restart from beginning of result set                               */
/***********************************************************************/
int ODBConn::Rewind(char *sql, ODBCCOL *tocols)
  {
  int rc, rbuf = -1;

  if (!m_hstmt)
    rbuf = -1;
  else if (m_Full)
    rbuf = m_Rows;           // No need to "rewind"
  else if (m_Scrollable) {
    SQLULEN  crow;

    try {
      rc = SQLExtendedFetch(m_hstmt, SQL_FETCH_FIRST, 1, &crow, NULL);

      if (!Check(rc))
        ThrowDBX(rc, "SQLExtendedFetch", m_hstmt);

      rbuf = (int)crow;
    } catch(DBX *x) {
      sprintf(m_G->Message, "%s: %s", x->m_Msg, x->GetErrorMessage(0));
      rbuf = -1;
    } // end try/catch

  } else if (ExecDirectSQL(sql, tocols) >= 0)
    rbuf = 0;

  return rbuf;
  } // end of Rewind

/***********************************************************************/
/*  Disconnect connection                                              */
/***********************************************************************/
void ODBConn::Close()
  {
  RETCODE rc;

  if (m_hstmt) {
    // Is required for multiple tables
    rc = SQLFreeStmt(m_hstmt, SQL_DROP);
    m_hstmt = NULL;
    } // endif m_hstmt
                                       
  if (m_hdbc != SQL_NULL_HDBC) {
    if (m_Transact) {
      rc = SQLEndTran(SQL_HANDLE_DBC, m_hdbc, SQL_COMMIT);
      m_Transact = false;
      } // endif m_Transact

    rc = SQLDisconnect(m_hdbc);

    if (trace(1) && rc != SQL_SUCCESS)
      htrc("Error: SQLDisconnect rc=%d\n", rc);

    rc = SQLFreeConnect(m_hdbc);

    if (trace(1) && rc != SQL_SUCCESS)
      htrc("Error: SQLFreeConnect rc=%d\n", rc);

    m_hdbc = SQL_NULL_HDBC;
    } // endif m_hdbc

  if (m_henv != SQL_NULL_HENV) {
    rc = SQLFreeEnv(m_henv);

    if (trace(1) && rc != SQL_SUCCESS)   // Nothing we can do
      htrc("Error: SQLFreeEnv failure ignored in Close\n");
          
    m_henv = SQL_NULL_HENV;
    } // endif m_henv

	if (m_Fp)
		m_Fp->Count = 0;

  } // end of Close
