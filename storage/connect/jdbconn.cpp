/************ Jdbconn C++ Functions Source Code File (.CPP) ************/
/*  Name: JDBCONN.CPP  Version 1.1                                     */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2016-2017    */
/*                                                                     */
/*  This file contains the JDBC connection classes functions.          */
/***********************************************************************/

#if defined(__WIN__)
// This is needed for RegGetValue
#define _WINVER 0x0601
#undef  _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif   // __WIN__

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
#endif   // __BORLANDC__
//#include <windows.h>
#else   // !__WIN__
#if defined(UNIX)
#include <errno.h>
#else   // !UNIX
//nclude <io.h>
#endif  // !UNIX
#include <stdio.h>
#include <stdlib.h>                      // for getenv
//nclude <fcntl.h>
#define NODW
#endif  // !__WIN__

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


#if defined(__WIN__)
extern "C" HINSTANCE s_hModule;           // Saved module handle
#endif   // __WIN__
#define nullptr 0

TYPCONV GetTypeConv();
int GetConvSize();
extern char *JvmPath;   // The connect_jvm_path global variable value
extern char *ClassPath; // The connect_class_path global variable value

char *GetJavaWrapper(void);		// The connect_java_wrapper variable value

/***********************************************************************/
/*  Static JDBConn objects.                                            */
/***********************************************************************/
void  *JDBConn::LibJvm = NULL;
CRTJVM JDBConn::CreateJavaVM = NULL;
GETJVM JDBConn::GetCreatedJavaVMs = NULL;
#if defined(_DEBUG)
GETDEF JDBConn::GetDefaultJavaVMInitArgs = NULL;
#endif   // _DEBUG

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
	case -1:  // LONGVARCHAR
		if (GetTypeConv() != TPC_YES)
			return TYPE_ERROR;
		else
		  len = MY_MIN(abs(len), GetConvSize());
	case 12:  // VARCHAR
		v = 'V';
	case 1:   // CHAR
		type = TYPE_STRING;
		break;
	case 2:   // NUMERIC
	case 3:   // DECIMAL
	case -3:  // VARBINARY
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
	case 93:  // TIMESTAMP, DATETIME
		type = TYPE_DATE;
		len = 19 + ((prec) ? (prec+1) : 0);
		v = (tn && toupper(tn[0]) == 'T') ? 'S' : 'E';
		break;
	case 91:  // DATE, YEAR
		type = TYPE_DATE;

		if (!tn || toupper(tn[0]) != 'Y') {
			len = 10;
			v = 'D';
		} else {
			len = 4;
			v = 'Y';
		}	// endif len

		break;
	case 92:  // TIME
		type = TYPE_DATE;
		len = 8 + ((prec) ? (prec+1) : 0);
		v = 'T';
		break;
	case -5:  // BIGINT
		type = TYPE_BIGINT;
		break;
	case 0:   // NULL
	case -2:  // BINARY
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
static JCATPARM *AllocCatInfo(PGLOBAL g, JCATINFO fid, PCSZ db,
	                            PCSZ tab, PQRYRES qrp)
{
	JCATPARM *cap;

#if defined(_DEBUG)
	assert(qrp);
#endif

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

		if (jcp->Open(sjp) != RC_OK)  // openReadOnly + noJDBCdialog
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

	// Colpat cannot be null or empty for some drivers
	cap->Pat = (colpat && *colpat) ? colpat : PlugDup(g, "%");

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
PQRYRES JDBCSrcCols(PGLOBAL g, PCSZ src, PJPARM sjp)
{
	char    *sqry;
	PQRYRES  qrp;
	JDBConn *jcp = new(g)JDBConn(g, NULL);

	if (jcp->Open(sjp))
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

		if (jcp->Open(sjp) == RC_FX)
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

	// Tabpat cannot be null or empty for some drivers
	if (!(cap = AllocCatInfo(g, CAT_TAB, db, 
	               (tabpat && *tabpat) ? tabpat : PlugDup(g, "%"), qrp)))
		return NULL;

	cap->Pat = tabtyp;

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

		if (jcp->Open(NULL) != RC_OK)
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

/***********************************************************************/
/*  JDBConn construction/destruction.                                  */
/***********************************************************************/
JDBConn::JDBConn(PGLOBAL g, TDBJDBC *tdbp)
{
	m_G = g;
	m_Tdb = tdbp;
	jvm = nullptr;            // Pointer to the JVM (Java Virtual Machine)
	env= nullptr;             // Pointer to native interface
	jdi = nullptr;						// Pointer to the java wrapper class
	job = nullptr;						// The java wrapper class object
	xqid = xuid = xid = grs = readid = fetchid = typid = errid = nullptr;
	prepid = xpid = pcid = nullptr;
	chrfldid = intfldid = dblfldid = fltfldid = bigfldid = nullptr;
	objfldid = datfldid = timfldid = tspfldid = nullptr;
	//m_LoginTimeout = DEFAULT_LOGIN_TIMEOUT;
//m_QueryTimeout = DEFAULT_QUERY_TIMEOUT;
//m_UpdateOptions = 0;
	Msg = NULL;
	m_Wrap = (tdbp && tdbp->WrapName) ? tdbp->WrapName : GetJavaWrapper();

	if (!strchr(m_Wrap, '/')) {
		// Add the wrapper package name
		char *wn = (char*)PlugSubAlloc(g, NULL, strlen(m_Wrap) + 10);
		m_Wrap = strcat(strcpy(wn, "wrappers/"), m_Wrap);
	} // endif m_Wrap

//m_Driver = NULL;
//m_Url = NULL;
//m_User = NULL;
//m_Pwd = NULL;
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

/***********************************************************************/
/*  Screen for errors.                                                 */
/***********************************************************************/
bool JDBConn::Check(jint rc)
{
	jstring s;

	if (env->ExceptionCheck()) {
		jthrowable exc = env->ExceptionOccurred();
		jmethodID tid = env->GetMethodID(env->FindClass("java/lang/Object"),
			"toString", "()Ljava/lang/String;");

		if (exc != nullptr && tid != nullptr) {
			jstring s = (jstring)env->CallObjectMethod(exc, tid);
			const char *utf = env->GetStringUTFChars(s, (jboolean)false);
			env->DeleteLocalRef(s);
			Msg = PlugDup(m_G, utf);
		} else
			Msg = "Exception occured";

		env->ExceptionClear();
	} else if (rc < 0) {
		s = (jstring)env->CallObjectMethod(job, errid);
		Msg = (char*)env->GetStringUTFChars(s, (jboolean)false);
	}	else
		Msg = NULL;

	return (Msg != NULL);
} // end of Check

/***********************************************************************/
/*  Get MethodID if not exists yet.                                    */
/***********************************************************************/
bool  JDBConn::gmID(PGLOBAL g, jmethodID& mid, const char *name, const char *sig)
{
	if (mid == nullptr) {
		mid = env->GetMethodID(jdi, name, sig);

		if (Check()) {
			strcpy(g->Message, Msg);
			return true;
		} else
			return false;

	} else
		return false;

} // end of gmID

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
/*  Reset the JVM library.                                             */
/***********************************************************************/
void JDBConn::ResetJVM(void)
{
	if (LibJvm) {
#if defined(__WIN__)
		FreeLibrary((HMODULE)LibJvm);
#else   // !__WIN__
		dlclose(LibJvm);
#endif  // !__WIN__
		LibJvm = NULL;
		CreateJavaVM = NULL;
		GetCreatedJavaVMs	= NULL;
#if defined(_DEBUG)
		GetDefaultJavaVMInitArgs = NULL;
#endif   // _DEBUG
	} // endif LibJvm

} // end of ResetJVM

/***********************************************************************/
/*  Dynamically link the JVM library.                                  */
/*  The purpose of this function is to allow using the CONNECT plugin  */
/*  for other table types when the Java JDK is not installed.          */
/***********************************************************************/
bool JDBConn::GetJVM(PGLOBAL g)
{
	int ntry;

	if (!LibJvm) {
		char soname[512];

#if defined(__WIN__)
		for (ntry = 0; !LibJvm && ntry < 3; ntry++) {
			if (!ntry && JvmPath) {
				strcat(strcpy(soname, JvmPath), "\\jvm.dll");
				ntry = 3;		 // No other try
			} else if (ntry < 2 && getenv("JAVA_HOME")) {
				strcpy(soname, getenv("JAVA_HOME"));

				if (ntry == 1)
					strcat(soname, "\\jre");

				strcat(soname, "\\bin\\client\\jvm.dll");
			} else {
				// Try to find it through the registry
				char version[16];
				char javaKey[64] = "SOFTWARE\\JavaSoft\\Java Runtime Environment";
				LONG  rc;
				DWORD BufferSize = 16;

				strcpy(soname, "jvm.dll");		// In case it fails

				if ((rc = RegGetValue(HKEY_LOCAL_MACHINE, javaKey, "CurrentVersion",
					RRF_RT_ANY, NULL, (PVOID)&version, &BufferSize)) == ERROR_SUCCESS) {
					strcat(strcat(javaKey, "\\"), version);
					BufferSize = sizeof(soname);

					if ((rc = RegGetValue(HKEY_LOCAL_MACHINE, javaKey, "RuntimeLib",
						RRF_RT_ANY, NULL, (PVOID)&soname, &BufferSize)) != ERROR_SUCCESS)
						printf("RegGetValue: rc=%ld\n", rc);

				} // endif rc

				ntry = 3;		 // Try this only once
			} // endelse

			// Load the desired shared library
			LibJvm = LoadLibrary(soname);
		}	// endfor ntry

		// Get the needed entries
		if (!LibJvm) {
			char  buf[256];
			DWORD rc = GetLastError();

			sprintf(g->Message, MSG(DLL_LOAD_ERROR), rc, soname);
			FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
				FORMAT_MESSAGE_IGNORE_INSERTS, NULL, rc, 0,
				(LPTSTR)buf, sizeof(buf), NULL);
			strcat(strcat(g->Message, ": "), buf);
		} else if (!(CreateJavaVM = (CRTJVM)GetProcAddress((HINSTANCE)LibJvm,
				                                               "JNI_CreateJavaVM"))) {
			sprintf(g->Message, MSG(PROCADD_ERROR), GetLastError(), "JNI_CreateJavaVM");
			FreeLibrary((HMODULE)LibJvm);
			LibJvm = NULL;
		} else if (!(GetCreatedJavaVMs = (GETJVM)GetProcAddress((HINSTANCE)LibJvm,
			                                                      "JNI_GetCreatedJavaVMs"))) {
			sprintf(g->Message, MSG(PROCADD_ERROR), GetLastError(), "JNI_GetCreatedJavaVMs");
			FreeLibrary((HMODULE)LibJvm);
			LibJvm = NULL;
#if defined(_DEBUG)
		} else if (!(GetDefaultJavaVMInitArgs = (GETDEF)GetProcAddress((HINSTANCE)LibJvm,
			  "JNI_GetDefaultJavaVMInitArgs"))) {
			sprintf(g->Message, MSG(PROCADD_ERROR), GetLastError(),
				"JNI_GetDefaultJavaVMInitArgs");
			FreeLibrary((HMODULE)LibJvm);
			LibJvm = NULL;
#endif   // _DEBUG
		} // endif LibJvm
#else   // !__WIN__
		const char *error = NULL;

		for (ntry = 0; !LibJvm && ntry < 2; ntry++) {
			if (!ntry && JvmPath) {
				strcat(strcpy(soname, JvmPath), "/libjvm.so");
				ntry = 2;
			} else if (!ntry && getenv("JAVA_HOME")) {
				// TODO: Replace i386 by a better guess
				strcat(strcpy(soname, getenv("JAVA_HOME")), "/jre/lib/i386/client/libjvm.so");
			} else {	 // Will need LD_LIBRARY_PATH to be set
				strcpy(soname, "libjvm.so");
				ntry = 2;
			} // endelse

			LibJvm = dlopen(soname, RTLD_LAZY);
		} // endfor ntry

		// Load the desired shared library
		if (!LibJvm) {
			error = dlerror();
			sprintf(g->Message, MSG(SHARED_LIB_ERR), soname, SVP(error));
		} else if (!(CreateJavaVM = (CRTJVM)dlsym(LibJvm, "JNI_CreateJavaVM"))) {
			error = dlerror();
			sprintf(g->Message, MSG(GET_FUNC_ERR), "JNI_CreateJavaVM", SVP(error));
			dlclose(LibJvm);
			LibJvm = NULL;
		} else if (!(GetCreatedJavaVMs = (GETJVM)dlsym(LibJvm, "JNI_GetCreatedJavaVMs"))) {
			error = dlerror();
			sprintf(g->Message, MSG(GET_FUNC_ERR), "JNI_GetCreatedJavaVMs", SVP(error));
			dlclose(LibJvm);
			LibJvm = NULL;
#if defined(_DEBUG)
	  } else if (!(GetDefaultJavaVMInitArgs = (GETDEF)dlsym(LibJvm,
		    "JNI_GetDefaultJavaVMInitArgs"))) {
		  error = dlerror();
			sprintf(g->Message, MSG(GET_FUNC_ERR), "JNI_GetDefaultJavaVMInitArgs", SVP(error));
			dlclose(LibJvm);
			LibJvm = NULL;
#endif   // _DEBUG
	} // endif LibJvm
#endif  // !__WIN__

	} // endif LibJvm

	return LibJvm == NULL;
} // end of GetJVM

/***********************************************************************/
/*  Open: connect to a data source.                                    */
/***********************************************************************/
int JDBConn::Open(PJPARM sop)
{
	int      irc = RC_FX;
	bool		 err = false;
	jboolean jt = (trace > 0);
	PGLOBAL& g = m_G;

	// Link or check whether jvm library was linked
	if (GetJVM(g))
		return RC_FX;

	// Firstly check whether the jvm was already created
	JavaVM* jvms[1];
	jsize   jsz;
	jint    rc = GetCreatedJavaVMs(jvms, 1, &jsz);

	if (rc == JNI_OK && jsz == 1) {
		// jvm already existing
		jvm = jvms[0];
		rc = jvm->AttachCurrentThread((void**)&env, nullptr);

		if (rc != JNI_OK) {
			strcpy(g->Message, "Cannot attach jvm to the current thread");
			return RC_FX;
		} // endif rc

	} else {
		/*******************************************************************/
		/*  Create a new jvm																							 */
		/*******************************************************************/
		PSTRG    jpop = new(g)STRING(g, 512, "-Djava.class.path=.");
		char    *cp = NULL;
		char     sep;

#if defined(__WIN__)
		sep = ';';
#define N 1
//#define N 2
//#define N 3
#else
		sep = ':';
#define N 1
#endif

		// Java source will be compiled as a jar file installed in the plugin dir
		jpop->Append(sep);
		jpop->Append(GetPluginDir());
		jpop->Append("JdbcInterface.jar");

		// All wrappers are pre-compiled in JavaWrappers.jar in the plugin dir
		jpop->Append(sep);
		jpop->Append(GetPluginDir());
		jpop->Append("JavaWrappers.jar");

		//================== prepare loading of Java VM ============================
		JavaVMInitArgs vm_args;                        // Initialization arguments
		JavaVMOption* options = new JavaVMOption[N];   // JVM invocation options

		// where to find java .class
		if (ClassPath && *ClassPath) {
			jpop->Append(sep);
			jpop->Append(ClassPath);
		}	// endif ClassPath

		if ((cp = getenv("CLASSPATH"))) {
			jpop->Append(sep);
			jpop->Append(cp);
		} // endif cp

		if (trace) {
			htrc("ClassPath=%s\n", ClassPath);
			htrc("CLASSPATH=%s\n", cp);
			htrc("%s\n", jpop->GetStr());
		} // endif trace

		options[0].optionString =	jpop->GetStr();
#if N == 2
		options[1].optionString =	"-Xcheck:jni";
#endif
#if N == 3
		options[1].optionString =	"-Xms256M";
		options[2].optionString =	"-Xmx512M";
#endif
#if defined(_DEBUG)
		vm_args.version = JNI_VERSION_1_2;             // minimum Java version
		rc = GetDefaultJavaVMInitArgs(&vm_args);
#else
		vm_args.version = JNI_VERSION_1_6;             // minimum Java version
#endif   // _DEBUG
		vm_args.nOptions = N;                          // number of options
		vm_args.options = options;
		vm_args.ignoreUnrecognized = false; // invalid options make the JVM init fail

		//=============== load and initialize Java VM and JNI interface =============
		rc = CreateJavaVM(&jvm, (void**)&env, &vm_args);  // YES !!
		delete options;    // we then no longer need the initialisation options.

		switch (rc) {
		case JNI_OK:
			strcpy(g->Message, "VM successfully created");
			irc = RC_OK;
			break;
		case JNI_ERR:
			strcpy(g->Message, "Initialising JVM failed: unknown error");
			break;
		case JNI_EDETACHED:
			strcpy(g->Message, "Thread detached from the VM");
			break;
		case JNI_EVERSION:
			strcpy(g->Message, "JNI version error");
			break;
		case JNI_ENOMEM:
			strcpy(g->Message, "Not enough memory");
			break;
		case JNI_EEXIST:
			strcpy(g->Message, "VM already created");
			break;
		case JNI_EINVAL:
			strcpy(g->Message, "Invalid arguments");
			break;
		default:
			sprintf(g->Message, "Unknown return code %d", (int)rc);
			break;
		} // endswitch rc

		if (trace)
			htrc("%s\n", g->Message);

		if (irc != RC_OK)
			return irc;

		//=============== Display JVM version ===============
		jint ver = env->GetVersion();
		printf("JVM Version %d.%d\n", ((ver>>16)&0x0f), (ver&0x0f));
	} // endif rc

	// try to find the java wrapper class
	jdi = env->FindClass(m_Wrap);

	if (jdi == nullptr) {
		sprintf(g->Message, "ERROR: class %s not found!", m_Wrap);
		return RC_FX;
	} // endif jdi

#if 0		// Suppressed because it does not make any usable change
	if (b && jpath && *jpath) {
		// Try to add that path the the jvm class path
		jmethodID alp =	env->GetStaticMethodID(jdi, "addLibraryPath",
			"(Ljava/lang/String;)I");

		if (alp == nullptr) {
			env->ExceptionDescribe();
			env->ExceptionClear();
		} else {
			char *msg;
			jstring path = env->NewStringUTF(jpath);
			rc = env->CallStaticIntMethod(jdi, alp, path);

			if ((msg = Check(rc))) {
				strcpy(g->Message, msg);
				env->DeleteLocalRef(path);
				return RC_FX;
			} else switch (rc) {
				case JNI_OK:
					printf("jpath added\n");
					break;
				case JNI_EEXIST:
					printf("jpath already exist\n");
					break;
				case JNI_ERR:
				default:
					strcpy(g->Message, "Error adding jpath");
					env->DeleteLocalRef(path);
					return RC_FX;
				}	// endswitch rc

			env->DeleteLocalRef(path);
		}	// endif alp

	}	// endif jpath
#endif // 0

	// if class found, continue
	jmethodID ctor = env->GetMethodID(jdi, "<init>", "(Z)V");

	if (ctor == nullptr) {
		sprintf(g->Message, "ERROR: %s constructor not found!", m_Wrap);
		return RC_FX;
	} else
		job = env->NewObject(jdi, ctor, jt);

	// If the object is successfully constructed, 
	// we can then search for the method we want to call, 
	// and invoke it for the object:
	if (job == nullptr) {
		sprintf(g->Message, "%s class object not constructed!", m_Wrap);
		return RC_FX;
	} // endif job

	errid = env->GetMethodID(jdi, "GetErrmsg", "()Ljava/lang/String;");

	if (env->ExceptionCheck()) {
		strcpy(g->Message, "ERROR: method GetErrmsg() not found!");
		env->ExceptionDescribe();
		env->ExceptionClear();
		return RC_FX;
	} // endif Check

	if (!sop)						 // DRIVER catalog table
		return RC_OK;

	jmethodID cid = nullptr;

	if (gmID(g, cid, "JdbcConnect", "([Ljava/lang/String;IZ)I"))
		return RC_FX;

	// Build the java string array
	jobjectArray parms = env->NewObjectArray(4,    // constructs java array of 4
		env->FindClass("java/lang/String"), NULL);   // Strings

//m_Driver = sop->Driver;
//m_Url = sop->Url;
//m_User = sop->User;
//m_Pwd = sop->Pwd;
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

//if (sop->Properties)
//	env->SetObjectArrayElement(parms, 4, env->NewStringUTF(sop->Properties));

	// call method
	rc = env->CallIntMethod(job, cid, parms, m_RowsetSize, m_Scrollable);
	err = Check(rc);
	env->DeleteLocalRef(parms);				 	// Not used anymore

	if (err) {
		sprintf(g->Message, "Connecting: %s rc=%d", Msg, (int)rc);
		return RC_FX;
	}	// endif Msg

	jmethodID qcid = nullptr;

	if (!gmID(g, qcid, "GetQuoteString", "()Ljava/lang/String;")) {
		jstring s = (jstring)env->CallObjectMethod(job, qcid);

		if (s != nullptr) {
			char *qch = (char*)env->GetStringUTFChars(s, (jboolean)false);
			m_IDQuoteChar[0] = *qch;
		} else {
			s = (jstring)env->CallObjectMethod(job, errid);
			Msg = (char*)env->GetStringUTFChars(s, (jboolean)false);
		}	// endif s

	}	// endif qcid

	if (gmID(g, typid, "ColumnType", "(ILjava/lang/String;)I"))
		return RC_FX;
	else
		m_Opened = true;

	return RC_OK;
} // end of Open

/***********************************************************************/
/*  Execute an SQL command.                                            */
/***********************************************************************/
int JDBConn::ExecSQLcommand(PCSZ sql)
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
} // end of ExecSQLcommand

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
		jmethodID did = nullptr;

		// Could have been detached in case of join
		rc = jvm->AttachCurrentThread((void**)&env, nullptr);

		if (gmID(m_G, did, "JdbcDisconnect", "()I"))
			printf("%s\n", Msg);
		else if (Check(env->CallIntMethod(job, did)))
			printf("jdbcDisconnect: %s\n", Msg);

		if ((rc = jvm->DetachCurrentThread()) != JNI_OK)
			printf("DetachCurrentThread: rc=%d\n", (int)rc);

		//rc = jvm->DestroyJavaVM();
		m_Opened = false;
	}	// endif m_Opened

} // end of Close

/***********************************************************************/
/*  Retrieve and set the column value from the result set.             */
/***********************************************************************/
void JDBConn::SetColumnValue(int rank, PSZ name, PVAL val)
{
	PGLOBAL& g = m_G;
	jint     ctyp;
	jstring  cn, jn = nullptr;
	jobject  jb = nullptr;

	if (rank == 0)
		if (!name || (jn = env->NewStringUTF(name)) == nullptr) {
			sprintf(g->Message, "Fail to allocate jstring %s", SVP(name));
			throw TYPE_AM_JDBC;
		}	// endif name

	// Returns 666 is case of error
	ctyp = env->CallIntMethod(job, typid, rank, jn);

	if (Check((ctyp == 666) ? -1 : 1)) {
		sprintf(g->Message, "Getting ctyp: %s", Msg);
		throw TYPE_AM_JDBC;
	} // endif Check

	if (val->GetNullable())
		if (!gmID(g, objfldid, "ObjectField", "(ILjava/lang/String;)Ljava/lang/Object;")) {
			jb = env->CallObjectMethod(job, objfldid, (jint)rank, jn);

			if (jb == nullptr) {
				val->Reset();
				val->SetNull(true);
				goto chk;
			}	// endif job

		}	// endif objfldid

	switch (ctyp) {
	case 12:          // VARCHAR
	case -1:          // LONGVARCHAR
	case 1:           // CHAR
  case 3:           // DECIMAL
		if (jb && ctyp != 3)
			cn = (jstring)jb;
		else if (!gmID(g, chrfldid, "StringField", "(ILjava/lang/String;)Ljava/lang/String;"))
			cn = (jstring)env->CallObjectMethod(job, chrfldid, (jint)rank, jn);
		else
			cn = nullptr;

		if (cn) {
			const char *field = env->GetStringUTFChars(cn, (jboolean)false);
			val->SetValue_psz((PSZ)field);
		} else
			val->Reset();

		break;
	case 4:           // INTEGER
	case 5:           // SMALLINT
	case -6:          // TINYINT
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
	case 0:						// NULL
		val->SetNull(true);
		// passthru
	default:
		val->Reset();
	} // endswitch Type

 chk:
	if (Check()) {
		if (rank == 0)
			env->DeleteLocalRef(jn);

		sprintf(g->Message, "SetColumnValue: %s rank=%d ctyp=%d", Msg, rank, (int)ctyp);
		throw TYPE_AM_JDBC;
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
int JDBConn::GetResultSize(PCSZ sql, JDBCCOL *colp)
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

		jrc = env->CallIntMethod(job, setid, i, (jint)GetJDBCType(val->GetType()));
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

		// Build the java string array
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
//	void    *buffer;
		int      i, ncol;
		PCSZ     fnc = "Unknown";
		uint     n;
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

    if (trace)
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
        if (trace)
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
	PQRYRES JDBConn::AllocateResult(PGLOBAL g)
	{
		bool         uns;
		PJDBCCOL     colp;
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
