/************ Jdbconn C++ Functions Source Code File (.CPP) ************/
/*  Name: JDBCONN.CPP  Version 1.2                                     */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2016-2018    */
/*                                                                     */
/*  This file contains the JDBC connection classes functions.          */
/***********************************************************************/

#if defined(_WIN32)
// This is needed for RegGetValue
#define _WINVER 0x0601
#undef  _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif   // _WIN32

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
#endif   // __BORLANDC__
//#include <windows.h>
#else   // !_WIN32
#if defined(UNIX)
#include <errno.h>
#else   // !UNIX
//nclude <io.h>
#endif  // !UNIX
#include <stdio.h>
#include <stdlib.h>                      // for getenv
//nclude <fcntl.h>
#define NODW
#endif  // !_WIN32

/***********************************************************************/
/*  Required objects includes.                                         */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xobject.h"
#include "xtable.h"
#include "tabext.h"
#include "tabjdbc.h"
//#include "jdbconn.h"
#include "resource.h"
#include "valblk.h"
#include "osutil.h"


//#if defined(_WIN32)
//extern "C" HINSTANCE s_hModule;           // Saved module handle
//#endif   // _WIN32
#define nullptr 0

TYPCONV GetTypeConv();
int GetConvSize();
//extern char *JvmPath;   // The connect_jvm_path global variable value
//extern char *ClassPath; // The connect_class_path global variable value

//char *GetJavaWrapper(void);		// The connect_java_wrapper variable value

/***********************************************************************/
/*  Some macro's (should be defined elsewhere to be more accessible)   */
/***********************************************************************/
//#if defined(_DEBUG)
//#define ASSERT(f)          assert(f)
//#define DEBUG_ONLY(f)      (f)
//#else   // !_DEBUG
//#define ASSERT(f)          ((void)0)
//#define DEBUG_ONLY(f)      ((void)0)
//#endif  // !_DEBUG

// To avoid gcc warning
int TranslateJDBCType(int stp, char *tn, int prec, int& len, char& v);

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
int TranslateJDBCType(int stp, char *tn, int prec, int& len, char& v)
{
	int type;

	switch (stp) {
	case -1:   // LONGVARCHAR, TEXT
	case -16:  // LONGNVARCHAR, NTEXT	(unicode)
		if (GetTypeConv() != TPC_YES)
			return TYPE_ERROR;
		else
		  len = MY_MIN(abs(len), GetConvSize());

                /* fall through */
	case 12:   // VARCHAR
		if (tn && !stricmp(tn, "TEXT"))
			// Postgresql returns 12 for TEXT
			if (GetTypeConv() == TPC_NO)
				return TYPE_ERROR;

		// Postgresql can return this 
		if (len == 0x7FFFFFFF)
			len = GetConvSize();

                /* fall through */
	case -9:   // NVARCHAR	(unicode)
		// Postgresql can return this when size is unknown 
		if (len == 0x7FFFFFFF)
			len = GetConvSize();

		v = 'V';
                /* fall through */
	case 1:    // CHAR
	case -15:  // NCHAR	 (unicode)
	case -8:   // ROWID
		type = TYPE_STRING;
		break;
	case 2:    // NUMERIC
	case 3:    // DECIMAL
	case -3:   // VARBINARY
		type = TYPE_DECIM;
		break;
	case 4:    // INTEGER
		type = TYPE_INT;
		break;
	case 5:    // SMALLINT
		type = TYPE_SHORT;
		break;
	case -6:   // TINYINT
	case -7:   // BIT
	case 16:   // BOOLEAN
		type = TYPE_TINY;
		break;
	case 6:    // FLOAT
	case 7:    // REAL
	case 8:    // DOUBLE
		type = TYPE_DOUBLE;
		break;
	case 93:   // TIMESTAMP, DATETIME
		type = TYPE_DATE;
		len = 19 + ((prec) ? (prec+1) : 0);
		v = (tn && toupper(tn[0]) == 'T') ? 'S' : 'E';
		break;
	case 91:   // DATE, YEAR
		type = TYPE_DATE;

			if (!tn || toupper(tn[0]) != 'Y') {
				len = 10;
				v = 'D';
			} else {
				len = 4;
				v = 'Y';
			}	// endif len

			break;
		case 92:   // TIME
			type = TYPE_DATE;
			len = 8 + ((prec) ? (prec + 1) : 0);
			v = 'T';
			break;
		case -5:   // BIGINT
			type = TYPE_BIGINT;
			break;
		case 1111: // UNKNOWN or UUID
			if (!tn || !stricmp(tn, "UUID")) {
				type = TYPE_STRING;
				len = 36;
				break;
			}	// endif tn

                        /* fall through */
		case 0:    // NULL
		case -2:   // BINARY
		case -4:   // LONGVARBINARY
		case 70:   // DATALINK
		case 2000: // JAVA_OBJECT
		case 2001: // DISTINCT
		case 2002: // STRUCT
		case 2003: // ARRAY
		case 2004: // BLOB
		case 2005: // CLOB
		case 2006: // REF
		case 2009: // SQLXML
		case 2011: // NCLOB
		default:
			type = TYPE_ERROR;
		len = 0;
	} // endswitch type

	return type;
} // end of TranslateJDBCType

	/***********************************************************************/
	/*  A helper class to split an optionally qualified table name into    */
	/*  components.                                                        */
	/*  These formats are understood:                                      */
	/*    "CatalogName.SchemaName.TableName"                               */
	/*    "SchemaName.TableName"                                           */
	/*    "TableName"                                                      */
	/***********************************************************************/
class SQLQualifiedName {
	static const uint max_parts = 3;          // Catalog.Schema.Table
	MYSQL_LEX_STRING m_part[max_parts];
	char m_buf[512];

	void lex_string_set(MYSQL_LEX_STRING *S, char *str, size_t length)
	{
		S->str = str;
		S->length = length;
	} // end of lex_string_set

	void lex_string_shorten_down(MYSQL_LEX_STRING *S, size_t offs)
	{
		DBUG_ASSERT(offs <= S->length);
		S->str += offs;
		S->length -= offs;
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
		for (i = S->length; i > 0; i--)
		{
			if (S->str[i - 1] == '.')
			{
				S->str[i - 1] = '\0';
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
			for (i = 1; i < max_parts; i++) {
				if (!(len = lex_string_find_qualifier(&m_part[i - 1])))
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
/*  Allocate the structure used to refer to the result set.            */
/***********************************************************************/
static JCATPARM *AllocCatInfo(PGLOBAL g, JCATINFO fid, PCSZ db,
	                            PCSZ tab, PQRYRES qrp)
{
	JCATPARM *cap;

	if ((cap = (JCATPARM *)PlgDBSubAlloc(g, NULL, sizeof(JCATPARM)))) {
		memset(cap, 0, sizeof(JCATPARM));
		cap->Id = fid;
		cap->Qrp = qrp;
		cap->DB = db;
		cap->Tab = tab;
	} // endif cap

	return cap;
} // end of AllocCatInfo

/***********************************************************************/
/*  JDBCColumns: constructs the result blocks containing all columns   */
/*  of a JDBC table that will be retrieved by GetData commands.        */
/***********************************************************************/
PQRYRES JDBCColumns(PGLOBAL g, PCSZ db, PCSZ table, PCSZ colpat,
	                             int maxres, bool info, PJPARM sjp)
{
	int  buftyp[] = {TYPE_STRING, TYPE_STRING, TYPE_STRING, TYPE_STRING,
									 TYPE_SHORT,  TYPE_STRING, TYPE_INT,    TYPE_INT,
									 TYPE_SHORT,  TYPE_SHORT,  TYPE_SHORT,  TYPE_STRING};
	XFLD fldtyp[] = {FLD_CAT,   FLD_SCHEM,    FLD_TABNAME, FLD_NAME,
								   FLD_TYPE,  FLD_TYPENAME, FLD_PREC,    FLD_LENGTH,
								   FLD_SCALE, FLD_RADIX,    FLD_NULL,    FLD_REM};
	unsigned int length[] = {0, 0, 0, 0, 6, 0, 10, 10, 6, 6, 6, 0};
	bool     b[] = {true, true, false, false, false, false, false, false, true, true, false, true};
	int       i, n, ncol = 12;
	PCOLRES   crp;
	PQRYRES   qrp;
	JCATPARM *cap;
	JDBConn  *jcp = NULL;

	/************************************************************************/
	/*  Do an evaluation of the result size.                                */
	/************************************************************************/
	if (!info) {
		jcp = new(g)JDBConn(g, NULL);

		if (jcp->Connect(sjp))  // openReadOnly + noJDBCdialog
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

	if (trace(1))
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

	if (trace(1))
		htrc("Getting col results ncol=%d\n", qrp->Nbcol);

	if (!(cap = AllocCatInfo(g, JCAT_COL, db, table, qrp)))
		return NULL;

	// Colpat cannot be null or empty for some drivers
	cap->Pat = (colpat && *colpat) ? colpat : PlugDup(g, "%");

	/************************************************************************/
	/*  Now get the results into blocks.                                    */
	/************************************************************************/
	if ((n = jcp->GetCatInfo(cap)) >= 0) {
		qrp->Nblin = n;
		//  ResetNullValues(cap);

		if (trace(1))
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
PQRYRES JDBCSrcCols(PGLOBAL g, PCSZ src, PJPARM sjp)
{
	char    *sqry;
	PQRYRES  qrp;
	JDBConn *jcp = new(g)JDBConn(g, NULL);

	if (jcp->Connect(sjp))
		return NULL;

	if (strstr(src, "%s")) {
		// Place holder for an eventual where clause
		sqry = (char*)PlugSubAlloc(g, NULL, strlen(src) + 2);
		sprintf(sqry, src, "1=1");			 // dummy where clause
	} else
		sqry = (char*)src;

	qrp = jcp->GetMetaData(g, sqry);
	jcp->Close();
	return qrp;
} // end of JDBCSrcCols

/**************************************************************************/
/*  JDBCTables: constructs the result blocks containing all tables in     */
/*  an JDBC database that will be retrieved by GetData commands.          */
/**************************************************************************/
PQRYRES JDBCTables(PGLOBAL g, PCSZ db, PCSZ tabpat, PCSZ tabtyp,
	                            int maxres, bool info, PJPARM sjp)
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

		if (jcp->Connect(sjp))
			return NULL;

		if (!maxres)
			maxres = 10000;                 // This is completely arbitrary

		n = jcp->GetMaxValue(2);					// Max catalog name length

//	if (n < 0)
//		return NULL;

		length[0] = (n > 0) ? (n + 1) : 0;
		n = jcp->GetMaxValue(3);					// Max schema name length
		length[1] = (n > 0) ? (n + 1) : 0;
		n = jcp->GetMaxValue(4);					// Max table name length
		length[2] = (n > 0) ? (n + 1) : 128;
	} else {
		maxres = 0;
		length[0] = 128;
		length[1] = 128;
		length[2] = 128;
		length[4] = 255;
	} // endif info

	if (trace(1))
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

	// Tabpat cannot be null or empty for some drivers
	if (!(cap = AllocCatInfo(g, JCAT_TAB, db, 
	               (tabpat && *tabpat) ? tabpat : PlugDup(g, "%"), qrp)))
		return NULL;

	cap->Pat = tabtyp;

	if (trace(1))
		htrc("Getting table results ncol=%d\n", cap->Qrp->Nbcol);

	/************************************************************************/
	/*  Now get the results into blocks.                                    */
	/************************************************************************/
	if ((n = jcp->GetCatInfo(cap)) >= 0) {
		qrp->Nblin = n;
		//  ResetNullValues(cap);

		if (trace(1))
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
PQRYRES JDBCDrivers(PGLOBAL g, int maxres, bool info)
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

		if (jcp->Open(g) != RC_OK)
			return NULL;

		if (!maxres)
			maxres = 256;         // Estimated max number of drivers

	} else
		maxres = 0;

	if (trace(1))
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

/***********************************************************************/
/*  JDBConn construction/destruction.                                  */
/***********************************************************************/
JDBConn::JDBConn(PGLOBAL g, PCSZ wrapper) : JAVAConn(g, wrapper)
{
	xqid = xuid = xid = grs = readid = fetchid = typid = errid = nullptr;
	prepid = xpid = pcid = nullptr;
	chrfldid = intfldid = dblfldid = fltfldid = bigfldid = nullptr;
	objfldid = datfldid = timfldid = tspfldid = uidfldid = nullptr;
	DiscFunc = "JdbcDisconnect";
	m_Ncol = 0;
	m_Aff = 0;
	//m_Rows = 0;
	m_Fetch = 0;
	m_RowsetSize = 0;
	m_Updatable = true;
	m_Transact = false;
	m_Scrollable = false;
	m_Full = false;
	m_Opened = false;
	m_IDQuoteChar[0] = '"';
	m_IDQuoteChar[1] = 0;
} // end of JDBConn

/***********************************************************************/
/*  Search for UUID columns.                                           */
/***********************************************************************/
bool JDBConn::SetUUID(PGLOBAL g, PTDBJDBC tjp)
{
	int          ncol, ctyp;
	bool         brc = true;
	PCSZ         fnc = "GetColumns";
	PCOL		     colp;
	JCATPARM    *cap;
	//jint         jtyp;
	jboolean     rc = false;
	jobjectArray parms;
	jmethodID    catid = nullptr;

	if (gmID(g, catid, fnc, "([Ljava/lang/String;)I"))
		return true;
	else if (gmID(g, intfldid, "IntField", "(ILjava/lang/String;)I"))
		return true;
	else if (gmID(g, readid, "ReadNext", "()I"))
		return true;

	cap = AllocCatInfo(g, JCAT_COL, tjp->Schema, tjp->TableName, NULL);
	SQLQualifiedName name(cap);

	// Build the java string array
	parms = env->NewObjectArray(4, env->FindClass("java/lang/String"), NULL);
	env->SetObjectArrayElement(parms, 0, env->NewStringUTF(name.ptr(2)));
	env->SetObjectArrayElement(parms, 1, env->NewStringUTF(name.ptr(1)));
	env->SetObjectArrayElement(parms, 2, env->NewStringUTF(name.ptr(0)));

	for (colp = tjp->GetColumns(); colp; colp = colp->GetNext()) {
		env->SetObjectArrayElement(parms, 3, env->NewStringUTF(colp->GetName()));
		ncol = env->CallIntMethod(job, catid, parms);

		if (Check(ncol)) {
			sprintf(g->Message, "%s: %s", fnc, Msg);
			goto err;
		}	// endif Check

		rc = env->CallBooleanMethod(job, readid);

		if (Check(rc)) {
			sprintf(g->Message, "ReadNext: %s", Msg);
			goto err;
		} else if (rc == 0) {
			sprintf(g->Message, "table %s does not exist", tjp->TableName);
			goto err;
		}	// endif rc

		// Should return 666 is case of error	(not done yet)
		ctyp = (int)env->CallIntMethod(job, intfldid, 5, nullptr);

		//if (Check((ctyp == 666) ? -1 : 1)) {
		//	sprintf(g->Message, "Getting ctyp: %s", Msg);
		//	goto err;
		//} // endif ctyp

		if (ctyp == 1111)
			((PJDBCCOL)colp)->uuid = true;

	} // endfor colp

	// All is Ok
	brc = false;

 err:
	// Not used anymore
	env->DeleteLocalRef(parms);
	return brc;
} // end of SetUUID

/***********************************************************************/
/*  Utility routine.                                                   */
/***********************************************************************/
int JDBConn::GetMaxValue(int n)
{
	jint      m;
	jmethodID maxid = nullptr;

	if (gmID(m_G, maxid, "GetMaxValue", "(I)I"))
		return -1;

	// call method
	if (Check(m = env->CallIntMethod(job, maxid, n)))
		htrc("GetMaxValue: %s", Msg);

	return (int)m;
} // end of GetMaxValue

/***********************************************************************/
/*  AddJars: add some jar file to the Class path.                      */
/***********************************************************************/
void JDBConn::AddJars(PSTRG jpop, char sep)
{
#if defined(DEVELOPMENT)
	jpop->Append(
		";C:/Jconnectors/postgresql-9.4.1208.jar"
		";C:/Oracle/ojdbc7.jar"
		";C:/Apache/commons-dbcp2-2.1.1/commons-dbcp2-2.1.1.jar"
		";C:/Apache/commons-pool2-2.4.2/commons-pool2-2.4.2.jar"
		";C:/Apache/commons-logging-1.2/commons-logging-1.2.jar"
		";C:/Jconnectors/mysql-connector-java-6.0.2-bin.jar"
		";C:/Jconnectors/mariadb-java-client-2.0.1.jar"
		";C:/Jconnectors/sqljdbc42.jar");
#endif   // DEVELOPMENT
} // end of AddJars

/***********************************************************************/
/*  Connect: connect to a data source.                                 */
/***********************************************************************/
bool JDBConn::Connect(PJPARM sop)
{
	bool		 err = false;
	jint     rc;
	PGLOBAL& g = m_G;

	/*******************************************************************/
	/*  Create or attach a JVM. 																			 */
	/*******************************************************************/
	if (Open(g))
		return true;

	if (!sop)						 // DRIVER catalog table
		return false;

	jmethodID cid = nullptr;

	if (gmID(g, cid, "JdbcConnect", "([Ljava/lang/String;IZ)I"))
		return true;

	// Build the java string array
	jobjectArray parms = env->NewObjectArray(4,    // constructs java array of 4
		env->FindClass("java/lang/String"), NULL);   // Strings

	m_Scrollable = sop->Scrollable;
	m_RowsetSize = sop->Fsize;
	//m_LoginTimeout = sop->Cto;
	//m_QueryTimeout = sop->Qto;
	//m_UseCnc = sop->UseCnc;

	// change some elements
	if (sop->Driver)
		env->SetObjectArrayElement(parms, 0, env->NewStringUTF(sop->Driver));

	if (sop->Url)
		env->SetObjectArrayElement(parms, 1, env->NewStringUTF(sop->Url));

	if (sop->User)
		env->SetObjectArrayElement(parms, 2, env->NewStringUTF(sop->User));

	if (sop->Pwd)
		env->SetObjectArrayElement(parms, 3, env->NewStringUTF(sop->Pwd));

	// call method
	rc = env->CallIntMethod(job, cid, parms, m_RowsetSize, m_Scrollable);
	err = Check(rc);
	env->DeleteLocalRef(parms);				 	// Not used anymore

	if (err) {
		sprintf(g->Message, "Connecting: %s rc=%d", Msg, (int)rc);
		return true;
	}	// endif Msg

	jmethodID qcid = nullptr;

	if (!gmID(g, qcid, "GetQuoteString", "()Ljava/lang/String;")) {
		jstring s = (jstring)env->CallObjectMethod(job, qcid);

		if (s != nullptr) {
			char *qch = GetUTFString(s);
			m_IDQuoteChar[0] = *qch;
		} else {
			s = (jstring)env->CallObjectMethod(job, errid);
			Msg = GetUTFString(s);
		}	// endif s

	}	// endif qcid

	if (gmID(g, typid, "ColumnType", "(ILjava/lang/String;)I"))
		return true;
	else
		m_Connected = true;

	return false;
} // end of Connect


/***********************************************************************/
/*  Execute an SQL command.                                            */
/***********************************************************************/
int JDBConn::ExecuteCommand(PCSZ sql)
{
	int      rc;
	jint     n;
	jstring  qry;
	PGLOBAL& g = m_G;

	// Get the methods used to execute a query and get the result
	if (gmID(g, xid, "Execute", "(Ljava/lang/String;)I") ||
			gmID(g, grs, "GetResult", "()I"))
		return RC_FX;

	qry = env->NewStringUTF(sql);
	n = env->CallIntMethod(job, xid, qry);
	env->DeleteLocalRef(qry);

	if (Check(n)) {
		sprintf(g->Message, "Execute: %s", Msg);
		return RC_FX;
	} // endif n

	m_Ncol = env->CallIntMethod(job, grs);

	if (Check(m_Ncol)) {
		sprintf(g->Message, "GetResult: %s", Msg);
		rc = RC_FX;
	} else if (m_Ncol) {
		strcpy(g->Message, "Result set column number");
		rc = RC_OK;						// A result set was returned
	} else {
		m_Aff = (int)n;			  // Affected rows
		strcpy(g->Message, "Affected rows");
		rc = RC_NF;
	} // endif ncol

	return rc;
} // end of ExecuteCommand

/***********************************************************************/
/*  Fetch next row.                                                    */
/***********************************************************************/
int JDBConn::Fetch(int pos)
{
	jint     rc = JNI_ERR;
	PGLOBAL& g = m_G;

	if (m_Full)						// Result set has one row
		return 1;

	if (pos) {
		if (!m_Scrollable) {
			strcpy(g->Message, "Cannot fetch(pos) if FORWARD ONLY");
		  return rc;
		} else if (gmID(m_G, fetchid, "Fetch", "(I)Z"))
			return rc;

		if (env->CallBooleanMethod(job, fetchid, pos))
			rc = m_Rows;

	} else {
		if (gmID(g, readid, "ReadNext", "()I"))
			return rc;

		rc = env->CallBooleanMethod(job, readid);

		if (!Check(rc)) {
			if (rc == 0)
				m_Full = (m_Fetch == 1);
			else
				m_Fetch++;

			m_Rows += (int)rc;
		} else
			sprintf(g->Message, "Fetch: %s", Msg);

	} // endif pos

	return (int)rc;
} // end of Fetch

/***********************************************************************/
/*  Restart from beginning of result set                               */
/***********************************************************************/
int JDBConn::Rewind(PCSZ sql)
{
	int rbuf = -1;

	if (m_Full)
		rbuf = m_Rows;           // No need to "rewind"
	else if (m_Scrollable) {
		if (gmID(m_G, fetchid, "Fetch", "(I)Z"))
			return -1;

		(void) env->CallBooleanMethod(job, fetchid, 0);

		rbuf = m_Rows;
	} else if (ExecuteCommand(sql) != RC_FX)
		rbuf = 0;

	return rbuf;
} // end of Rewind

/***********************************************************************/
/*  Retrieve and set the column value from the result set.             */
/***********************************************************************/
void JDBConn::SetColumnValue(int rank, PSZ name, PVAL val)
{
	const char *field;
	PGLOBAL& g = m_G;
	jint     ctyp;
	jstring  cn, jn = nullptr;
	jobject  jb = nullptr;

	if (rank == 0)
		if (!name || (jn = env->NewStringUTF(name)) == nullptr) {
			sprintf(g->Message, "Fail to allocate jstring %s", SVP(name));
			throw (int)TYPE_AM_JDBC;
		}	// endif name

	// Returns 666 is case of error
	ctyp = env->CallIntMethod(job, typid, rank, jn);

	if (Check((ctyp == 666) ? -1 : 1)) {
		sprintf(g->Message, "Getting ctyp: %s", Msg);
		throw (int)TYPE_AM_JDBC;
	} // endif Check

	if (val->GetNullable())
		if (!gmID(g, objfldid, "ObjectField", "(ILjava/lang/String;)Ljava/lang/Object;")) {
			jb = env->CallObjectMethod(job, objfldid, (jint)rank, jn);

			if (Check(0)) {
				sprintf(g->Message, "Getting jp: %s", Msg);
				throw (int)TYPE_AM_JDBC;
			} // endif Check

			if (jb == nullptr) {
				val->Reset();
				val->SetNull(true);
				goto chk;
			}	// endif job

		}	// endif objfldid

	switch (ctyp) {
	case 12:          // VARCHAR
	case -9:          // NVARCHAR
	case -1:          // LONGVARCHAR, TEXT
	case 1:           // CHAR
	case -15:         // NCHAR
	case -16:         // LONGNVARCHAR, NTEXT
	case 3:           // DECIMAL
	case -8:          // ROWID
		if (jb && ctyp != 3)
			cn = (jstring)jb;
		else if (!gmID(g, chrfldid, "StringField", "(ILjava/lang/String;)Ljava/lang/String;"))
			cn = (jstring)env->CallObjectMethod(job, chrfldid, (jint)rank, jn);
		else
			cn = nullptr;

		if (cn) {
			field = GetUTFString(cn);
			val->SetValue_psz((PSZ)field);
		} else
			val->Reset();

		break;
	case 4:           // INTEGER
	case 5:           // SMALLINT
	case -6:          // TINYINT
	case 16:          // BOOLEAN
	case -7:          // BIT
		if (!gmID(g, intfldid, "IntField", "(ILjava/lang/String;)I"))
			val->SetValue((int)env->CallIntMethod(job, intfldid, rank, jn));
		else
			val->Reset();

		break;
	case 8:           // DOUBLE
	case 2:           // NUMERIC
//case 3:           // DECIMAL
		if (!gmID(g, dblfldid, "DoubleField", "(ILjava/lang/String;)D"))
			val->SetValue((double)env->CallDoubleMethod(job, dblfldid, rank, jn));
		else
			val->Reset();

		break;
	case 7:           // REAL
	case 6:           // FLOAT
		if (!gmID(g, fltfldid, "FloatField", "(ILjava/lang/String;)F"))
			val->SetValue((float)env->CallFloatMethod(job, fltfldid, rank, jn));
		else
			val->Reset();

		break;
	case 91:          // DATE
		if (!gmID(g, datfldid, "DateField", "(ILjava/lang/String;)I")) {
			val->SetValue((int)env->CallIntMethod(job, datfldid, (jint)rank, jn));
		} else
			val->Reset();

		break;
	case 92:          // TIME
		if (!gmID(g, timfldid, "TimeField", "(ILjava/lang/String;)I")) {
			val->SetValue((int)env->CallIntMethod(job, timfldid, (jint)rank, jn));
		} else
			val->Reset();

		break;
	case 93:          // TIMESTAMP
		if (!gmID(g, tspfldid, "TimestampField", "(ILjava/lang/String;)I")) {
			val->SetValue((int)env->CallIntMethod(job, tspfldid, (jint)rank, jn));
		} else
			val->Reset();

		break;
	case -5:          // BIGINT
		if (!gmID(g, bigfldid, "BigintField", "(ILjava/lang/String;)J"))
			val->SetValue((long long)env->CallLongMethod(job, bigfldid, (jint)rank, jn));
		else
			val->Reset();

		break;
		/*			case java.sql.Types.SMALLINT:
		System.out.print(jdi.IntField(i));
		break;
		case java.sql.Types.BOOLEAN:
		System.out.print(jdi.BooleanField(i)); */
	case 1111:				// UUID
		if (!gmID(g, uidfldid, "UuidField", "(ILjava/lang/String;)Ljava/lang/String;"))
			cn = (jstring)env->CallObjectMethod(job, uidfldid, (jint)rank, jn);
		else
			cn = nullptr;

		if (cn) {
			val->SetValue_psz((PSZ)GetUTFString(cn));
		} else
			val->Reset();

		break;
	case 0:						// NULL
		val->SetNull(true);
                /* fall through */
	default:
		val->Reset();
	} // endswitch Type

 chk:
	if (Check()) {
		if (rank == 0)
			env->DeleteLocalRef(jn);

		sprintf(g->Message, "SetColumnValue: %s rank=%d ctyp=%d", Msg, rank, (int)ctyp);
		throw (int)TYPE_AM_JDBC;
	} // endif Check

	if (rank == 0)
		env->DeleteLocalRef(jn);

} // end of SetColumnValue

/***********************************************************************/
/*  Prepare an SQL statement for insert.                               */
/***********************************************************************/
bool JDBConn::PrepareSQL(PCSZ sql)
{
	bool		 b = true;
	PGLOBAL& g = m_G;

	if (!gmID(g, prepid, "CreatePrepStmt", "(Ljava/lang/String;)I")) {
		// Create the prepared statement
		jstring qry = env->NewStringUTF(sql);

		if (Check(env->CallBooleanMethod(job, prepid, qry)))
			sprintf(g->Message, "CreatePrepStmt: %s", Msg);
		else
			b = false;

		env->DeleteLocalRef(qry);
	} // endif prepid

	return b;
} // end of PrepareSQL

/***********************************************************************/
/*  Execute an SQL query that returns a result set.                    */
/***********************************************************************/
int JDBConn::ExecuteQuery(PCSZ sql)
{
	int			 rc = RC_FX;
	jint     ncol;
	jstring  qry;
	PGLOBAL& g = m_G;

	// Get the methods used to execute a query and get the result
	if (!gmID(g, xqid, "ExecuteQuery", "(Ljava/lang/String;)I")) {
		qry = env->NewStringUTF(sql);
		ncol = env->CallIntMethod(job, xqid, qry);

		if (!Check(ncol)) {
			m_Ncol = (int)ncol;
			m_Aff = 0;			  // Affected rows
			rc = RC_OK;
		} else
			sprintf(g->Message, "ExecuteQuery: %s", Msg);

		env->DeleteLocalRef(qry);
	} // endif xqid

	return rc;
} // end of ExecuteQuery

/***********************************************************************/
/*  Execute an SQL query and get the affected rows.                    */
/***********************************************************************/
int JDBConn::ExecuteUpdate(PCSZ sql)
{
	int      rc = RC_FX;
	jint     n;
	jstring  qry;
	PGLOBAL& g = m_G;

	// Get the methods used to execute a query and get the affected rows
	if (!gmID(g, xuid, "ExecuteUpdate", "(Ljava/lang/String;)I")) {
		qry = env->NewStringUTF(sql);
		n = env->CallIntMethod(job, xuid, qry);

		if (!Check(n)) {
			m_Ncol = 0;
			m_Aff = (int)n;			  // Affected rows
			rc = RC_OK;
		} else
			sprintf(g->Message, "ExecuteUpdate: %s n=%d", Msg, n);

		env->DeleteLocalRef(qry);
	} // endif xuid

	return rc;
} // end of ExecuteUpdate

/***********************************************************************/
/*  Get the number of lines of the result set.                         */
/***********************************************************************/
int JDBConn::GetResultSize(PCSZ sql, PCOL colp)
{
	int rc;

	if ((rc = ExecuteQuery(sql)) != RC_OK)
		return -1;

	if ((rc = Fetch()) > 0) {
		try {
			SetColumnValue(1, NULL, colp->GetValue());
		}	catch (...) {
			return -4;
		} // end catch

	} else
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

	// Get the methods used to execute a prepared statement
	if (!gmID(g, xpid, "ExecutePrep", "()I")) {
		jint n = env->CallIntMethod(job, xpid);

		if (n == -3)
			strcpy(g->Message, "SQL statement is not prepared");
		else if (Check(n))
			sprintf(g->Message, "ExecutePrep: %s", Msg);
		else {
			m_Aff = (int)n;
			rc = RC_OK;
		} // endswitch n

	} // endif xpid

	return rc;
} // end of ExecuteSQL

/***********************************************************************/
/*  Set a parameter for inserting.                                     */
/***********************************************************************/
bool JDBConn::SetParam(JDBCCOL *colp)
{
	PGLOBAL&   g = m_G;
	bool       rc = false;
	PVAL       val = colp->GetValue();
	jint       n, jrc = 0, i = (jint)colp->GetRank();
	jshort     s;
	jlong      lg;
//jfloat     f;
	jdouble    d;
	jclass     dat;
	jobject    datobj;
	jstring    jst = nullptr;
	jmethodID  dtc, setid = nullptr;

	if (val->GetNullable() && val->IsNull()) {
		if (gmID(g, setid, "SetNullParm", "(II)I"))
			return true;

		jrc = env->CallIntMethod(job, setid, i, 
			(colp->uuid ? 1111 : (jint)GetJDBCType(val->GetType())));
	} else if (colp->uuid) {
		if (gmID(g, setid, "SetUuidParm", "(ILjava/lang/String;)V"))
			return true;

		jst = env->NewStringUTF(val->GetCharValue());
		env->CallVoidMethod(job, setid, i, jst);
	} else switch (val->GetType()) {
		case TYPE_STRING:
			if (gmID(g, setid, "SetStringParm", "(ILjava/lang/String;)V"))
				return true;

			jst = env->NewStringUTF(val->GetCharValue());
			env->CallVoidMethod(job, setid, i, jst);
			break;
		case TYPE_INT:
			if (gmID(g, setid, "SetIntParm", "(II)V"))
				return true;

			n = (jint)val->GetIntValue();
			env->CallVoidMethod(job, setid, i, n);
			break;
		case TYPE_TINY:
		case TYPE_SHORT:
			if (gmID(g, setid, "SetShortParm", "(IS)V"))
				return true;

			s = (jshort)val->GetShortValue();
			env->CallVoidMethod(job, setid, i, s);
			break;
		case TYPE_BIGINT:
			if (gmID(g, setid, "SetBigintParm", "(IJ)V"))
				return true;

			lg = (jlong)val->GetBigintValue();
			env->CallVoidMethod(job, setid, i, lg);
			break;
		case TYPE_DOUBLE:
		case TYPE_DECIM:
			if (gmID(g, setid, "SetDoubleParm", "(ID)V"))
				return true;

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
			} else if (gmID(g, setid, "SetTimestampParm", "(ILjava/sql/Timestamp;)V"))
				return true;

			env->CallVoidMethod(job, setid, i, datobj);
			break;
		default:
			sprintf(g->Message, "Parm type %d not supported", val->GetType());
			return true;
		}	// endswitch Type

	if (Check(jrc)) {
		sprintf(g->Message, "SetParam: col=%s msg=%s", colp->GetName(), Msg);
		rc = true;
	} // endif msg

	if (jst)
		env->DeleteLocalRef(jst);

	return rc;
	} // end of SetParam

	/***********************************************************************/
	/*  Get the list of Drivers and set it in qrp.                         */
	/***********************************************************************/
	bool JDBConn::GetDrivers(PQRYRES qrp)
	{
		PSZ       sval;
		int       i, n, size;
		PCOLRES   crp;
		jstring   js;
		jmethodID gdid = nullptr;
		
		if (gmID(m_G, gdid, "GetDrivers", "([Ljava/lang/String;I)I"))
			return true;

		// Build the java string array
		jobjectArray s = env->NewObjectArray(4 * qrp->Maxres,
			env->FindClass("java/lang/String"), NULL);

		size = env->CallIntMethod(job, gdid, s, qrp->Maxres);

		for (i = 0, n = 0; i < size; i++) {
			crp = qrp->Colresp;
			js = (jstring)env->GetObjectArrayElement(s, n++);
			sval = GetUTFString(js);
			crp->Kdata->SetValue(sval, i);
			crp = crp->Next;
			js = (jstring)env->GetObjectArrayElement(s, n++);
			sval = GetUTFString(js);
			crp->Kdata->SetValue(sval, i);
			crp = crp->Next;
			js = (jstring)env->GetObjectArrayElement(s, n++);
			sval = GetUTFString(js);
			crp->Kdata->SetValue(sval, i);
			crp = crp->Next;
			js = (jstring)env->GetObjectArrayElement(s, n++);
			sval = GetUTFString(js);
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
	PQRYRES JDBConn::GetMetaData(PGLOBAL g, PCSZ src)
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
		ushort  i;
		jint   *n = nullptr;
		jstring label;
		jmethodID colid = nullptr;
		int     rc = ExecuteCommand(src);

		if (rc == RC_NF) {
			strcpy(g->Message, "Srcdef is not returning a result set");
			return NULL;
		} else if ((rc) == RC_FX) {
			return NULL;
		} else if (m_Ncol == 0) {
			strcpy(g->Message, "Invalid Srcdef");
			return NULL;
		} // endif's

		if (gmID(g, colid, "ColumnDesc", "(I[I)Ljava/lang/String;"))
			return NULL;

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

		// Build the java int array
		jintArray val = env->NewIntArray(4);

		if (val == nullptr) {
			strcpy(m_G->Message, "Cannot allocate jint array");
			return NULL;
		} // endif colid

		/************************************************************************/
		/*  Now get the results into blocks.                                    */
		/************************************************************************/
		for (i = 0; i < m_Ncol; i++) {
			if (!(label = (jstring)env->CallObjectMethod(job, colid, i + 1, val))) {
				if (Check())
					sprintf(g->Message, "ColumnDesc: %s", Msg);
				else
					strcpy(g->Message, "No result metadata");

				env->ReleaseIntArrayElements(val, n, 0);
				return NULL;
			} // endif label

			name = GetUTFString(label);
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

		/************************************************************************/
		/*  Return the result pointer for use by GetData routines.              */
		/************************************************************************/
		return qrp;
	} // end of GetMetaData

	/***********************************************************************/
	/*  Allocate recset and call SQLTables, SQLColumns or SQLPrimaryKeys.  */
	/***********************************************************************/
	int JDBConn::GetCatInfo(JCATPARM *cap)
	{
		PGLOBAL& g = m_G;
//	void    *buffer;
		int      i, ncol;
		PCSZ     fnc = "Unknown";
		uint     n;
		short    len, tp;
		PQRYRES  qrp = cap->Qrp;
		PCOLRES  crp;
		jboolean rc = false;
//	HSTMT    hstmt = NULL;
//	SQLLEN  *vl, *vlen = NULL;
		PVAL    *pval = NULL;
		char*   *pbuf = NULL;
		jobjectArray parms;
		jmethodID    catid = nullptr;

		if (qrp->Maxres <= 0)
			return 0;				 			// 0-sized result"

		SQLQualifiedName name(cap);

		// Build the java string array
		parms = env->NewObjectArray(4, env->FindClass("java/lang/String"), NULL);
		env->SetObjectArrayElement(parms, 0, env->NewStringUTF(name.ptr(2)));
		env->SetObjectArrayElement(parms, 1, env->NewStringUTF(name.ptr(1)));
		env->SetObjectArrayElement(parms, 2, env->NewStringUTF(name.ptr(0)));
		env->SetObjectArrayElement(parms, 3, env->NewStringUTF((const char*)cap->Pat));

		// Now do call the proper JDBC API
		switch (cap->Id) {
		case JCAT_COL:
			fnc = "GetColumns";
			break;
		case JCAT_TAB:
			fnc = "GetTables";
			break;
#if 0
		case JCAT_KEY:
			fnc = "SQLPrimaryKeys";
			rc = SQLPrimaryKeys(hstmt, name.ptr(2), name.length(2),
				name.ptr(1), name.length(1),
				name.ptr(0), name.length(0));
			break;
#endif // 0
		default:
			sprintf(g->Message, "Invalid SQL function id");
			return -1;
		} // endswitch infotype

		if (gmID(g, catid, fnc, "([Ljava/lang/String;)I"))
			return -1;

		// call method
		ncol = env->CallIntMethod(job, catid, parms);

		if (Check(ncol)) {
			sprintf(g->Message, "%s: %s", fnc, Msg);
			env->DeleteLocalRef(parms);
			return -1;
		}	// endif Check

		// Not used anymore
		env->DeleteLocalRef(parms);

    if (trace(1))
      htrc("Method %s returned %d columns\n", fnc, ncol);

		// n because we no more ignore the first column
		if ((n = qrp->Nbcol) > (uint)ncol) {
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
			pval[n]->SetNullable(true);

			if (crp->Type == TYPE_STRING) {
				pbuf[n] = (char*)PlugSubAlloc(g, NULL, len);
//			buffer = pbuf[n];
      } // endif Type
//		} else
//			buffer = pval[n]->GetTo_Val();

			n++;
		} // endfor n

		// Now fetch the result
		for (i = 0; i < qrp->Maxres; i++) {
			if (Check(rc = Fetch(0))) {
				sprintf(g->Message, "Fetch: %s", Msg);
				return -1;
			} if (rc == 0) {
        if (trace(1))
          htrc("End of fetches i=%d\n", i);

				break;
			} // endif rc

			for (n = 0, crp = qrp->Colresp; crp; n++, crp = crp->Next) {
				SetColumnValue(n + 1, nullptr, pval[n]);
				crp->Kdata->SetValue(pval[n], i);
			}	// endfor n

		} // endfor i

		if (rc > 0)
			qrp->Truncated = true;

		return i;
	} // end of GetCatInfo

	/***********************************************************************/
	/*  Allocate a CONNECT result structure from the JDBC result.          */
	/***********************************************************************/
	PQRYRES JDBConn::AllocateResult(PGLOBAL g, PTDB tdbp)
	{
		bool         uns;
		PCOL         colp;
		PCOLRES     *pcrp, crp;
		PQRYRES      qrp;

		if (!m_Rows) {
			strcpy(g->Message, "Void result");
			return NULL;
		} // endif m_Rows

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

		for (colp = tdbp->GetColumns(); colp; colp = colp->GetNext())
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

				((EXTCOL*)colp)->SetCrp(crp);
			} // endif colp

		*pcrp = NULL;
		return qrp;
	} // end of AllocateResult
