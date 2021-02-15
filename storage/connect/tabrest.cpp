/************** tabrest C++ Program Source Code File (.CPP) ************/
/* PROGRAM NAME: tabrest   Version 1.8                                 */
/*  (C) Copyright to the author Olivier BERTRAND          2018 - 2020  */
/*  This program is the REST Web API support for MariaDB.              */
/*  When compiled without MARIADB defined, it is the EOM module code.  */
/*  The way Connect handles NOSQL data returned by REST queries is     */
/*  just by retrieving it as a file and then leave the existing data   */
/*  type tables (JSON, XML or CSV) process it as usual.                */
/***********************************************************************/

/***********************************************************************/
/*  Definitions needed by the included files.                          */
/***********************************************************************/
#if defined(MARIADB)
#include <my_global.h>    // All MariaDB stuff
#include <mysqld.h>
#include <sql_error.h>
#else   // !MARIADB       OEM module
#include "mini-global.h"
#define _MAX_PATH 260
#if !defined(REST_SOURCE)
#if defined(__WIN__) || defined(_WINDOWS)
#include <windows.h>
#else		 // !__WIN__
#define __stdcall
#include <dlfcn.h>         // dlopen(), dlclose(), dlsym() ...
#endif   // !__WIN__
#endif	 // !REST_SOURCE
#define _OS_H_INCLUDED     // Prevent os.h to be called
#endif  // !MARIADB

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xtable.h"
#include "filamtxt.h"
#include "tabdos.h"
#include "plgxml.h"
#if defined(XML_SUPPORT)
#include "tabxml.h"
#endif   // XML_SUPPORT
#include "tabjson.h"
#include "tabfmt.h"
#include "tabrest.h"

#if defined(connect_EXPORTS)
#define PUSH_WARNING(M) push_warning(current_thd, Sql_condition::WARN_LEVEL_WARN, 0, M)
#else
#define PUSH_WARNING(M) htrc(M)
#endif

#if defined(__WIN__) || defined(_WINDOWS)
#define popen  _popen
#define pclose _pclose
#endif

static XGETREST getRestFnc = NULL;
static int Xcurl(PGLOBAL g, PCSZ Http, PCSZ Uri, PCSZ filename);

#if !defined(MARIADB)
/***********************************************************************/
/*  DB static variables.                                               */
/***********************************************************************/
int    TDB::Tnum;
int    DTVAL::Shift;
int    CSORT::Limit = 0;
double CSORT::Lg2 = log(2.0);
size_t CSORT::Cpn[1000] = { 0 };

/***********************************************************************/
/*  These functions are exported from the REST library.                */
/***********************************************************************/
extern "C" {
  PTABDEF __stdcall GetREST(PGLOBAL, void*);
  PQRYRES __stdcall ColREST(PGLOBAL, PTOS, char*, char*, bool);
} // extern "C"

/***********************************************************************/
/*  This function returns a table definition class.                    */
/***********************************************************************/
PTABDEF __stdcall GetREST(PGLOBAL g, void *memp)
{
  return new(g, memp) RESTDEF;
} // end of GetREST
#endif   // !MARIADB

/***********************************************************************/
/*  Xcurl: retrieve the REST answer by executing cURL.                 */
/***********************************************************************/
int Xcurl(PGLOBAL g, PCSZ Http, PCSZ Uri, PCSZ filename)
{
	char  buf[1024];
	int   rc;
	FILE *pipe;

	if (Uri) {
		if (*Uri == '/' || Http[strlen(Http) - 1] == '/')
			sprintf(buf, "curl %s%s -o %s", Http, Uri, filename);
		else
			sprintf(buf, "curl %s/%s -o %s", Http, Uri, filename);

	} else
		sprintf(buf, "curl %s -o %s", Http, filename);

	if ((pipe = popen(buf, "rt"))) {
		if (trace(515))
			while (fgets(buf, sizeof(buf), pipe)) {
				htrc("%s", buf);
			}	// endwhile

		pclose(pipe);
		rc = 0;
	} else {
		sprintf(g->Message, "curl failed, errno =%d", errno);
		rc = 1;
	} // endif pipe

	return rc;
} // end od Xcurl

/***********************************************************************/
/*  GetREST: load the Rest lib and get the Rest function.              */
/***********************************************************************/
XGETREST GetRestFunction(PGLOBAL g)
{
	if (getRestFnc)
		return getRestFnc;
	
#if !defined(MARIADB) || !defined(REST_SOURCE)
	if (trace(515))
		htrc("Looking for GetRest library\n");

#if defined(__WIN__) || defined(_WINDOWS)
	HANDLE Hdll;
	const char* soname = "GetRest.dll";   // Module name

	if (!(Hdll = LoadLibrary(soname))) {
		char  buf[256];
		DWORD rc = GetLastError();

		sprintf(g->Message, MSG(DLL_LOAD_ERROR), rc, soname);
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS, NULL, rc, 0,
			(LPTSTR)buf, sizeof(buf), NULL);
		strcat(strcat(g->Message, ": "), buf);
		return NULL;
	} // endif Hdll

// Get the function returning an instance of the external DEF class
	if (!(getRestFnc = (XGETREST)GetProcAddress((HINSTANCE)Hdll, "restGetFile"))) {
		char  buf[256];
		DWORD rc = GetLastError();

		sprintf(g->Message, MSG(PROCADD_ERROR), rc, "restGetFile");
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS, NULL, rc, 0,
			(LPTSTR)buf, sizeof(buf), NULL);
		strcat(strcat(g->Message, ": "), buf);
		FreeLibrary((HMODULE)Hdll);
		return NULL;
	} // endif getRestFnc
#else   // !__WIN__
	void* Hso;
	const char* error = NULL;
	const char* soname = "GetRest.so";   // Module name

	// Load the desired shared library
	if (!(Hso = dlopen(soname, RTLD_LAZY))) {
		error = dlerror();
		sprintf(g->Message, MSG(SHARED_LIB_ERR), soname, SVP(error));
		return NULL;
	} // endif Hdll

// Get the function returning an instance of the external DEF class
	if (!(getRestFnc = (XGETREST)dlsym(Hso, "restGetFile"))) {
		error = dlerror();
		sprintf(g->Message, MSG(GET_FUNC_ERR), "restGetFile", SVP(error));
		dlclose(Hso);
		return NULL;
	} // endif getdef
#endif  // !__WIN__
#else
	getRestFnc = restGetFile;
#endif

	return getRestFnc;
} // end of GetRestFunction

/***********************************************************************/
/*  Return the columns definition to MariaDB.                          */
/***********************************************************************/
#if defined(MARIADB)
PQRYRES RESTColumns(PGLOBAL g, PTOS tp, char *tab, char *db, bool info)
#else   // !MARIADB
PQRYRES __stdcall ColREST(PGLOBAL g, PTOS tp, char *tab, char *db, bool info)
#endif  // !MARIADB
{
  PQRYRES  qrp= NULL;
  char     filename[_MAX_PATH + 1];  // MAX PATH ???
	int      rc;
	bool     curl = false;
  PCSZ     http, uri, fn, ftype;
	XGETREST grf = GetRestFunction(g);

	if (!grf)
		curl = true;

  http = GetStringTableOption(g, tp, "Http", NULL);
  uri = GetStringTableOption(g, tp, "Uri", NULL);
#if defined(MARIADB)
  ftype = GetStringTableOption(g, tp, "Type", "JSON");
#else   // !MARIADB
  // OEM tables must specify the file type
  ftype = GetStringTableOption(g, tp, "Ftype", "JSON");
#endif  // !MARIADB
	fn = GetStringTableOption(g, tp, "Filename", NULL);

	if (!fn) {
		int n, m = strlen(ftype) + 1;

		strcat(strcpy(filename, tab), ".");
		n = strlen(filename);

		// Fold ftype to lower case
		for (int i = 0; i < m; i++)
			filename[n + i] = tolower(ftype[i]);

		fn = filename;
		tp->filename = PlugDup(g, fn);
		sprintf(g->Message, "No file name. Table will use %s", fn);
		PUSH_WARNING(g->Message);
	}	// endif fn

  //  We used the file name relative to recorded datapath
	PlugSetPath(filename, fn, db);
	curl = GetBooleanTableOption(g, tp, "Curl", curl);

  // Retrieve the file from the web and copy it locally
	if (curl)
		rc = Xcurl(g, http, uri, filename);
	else if (grf)
		rc = grf(g->Message, trace(515), http, uri, filename);
	else {
		strcpy(g->Message, "Cannot access to curl nor casablanca");
		rc = 1;
	}	// endif !grf

	if (rc)
		return NULL;
  else if (!stricmp(ftype, "JSON"))
    qrp = JSONColumns(g, db, NULL, tp, info);
  else if (!stricmp(ftype, "CSV"))
    qrp = CSVColumns(g, NULL, tp, info);
#if defined(XML_SUPPORT)
	else if (!stricmp(ftype, "XML"))
		qrp = XMLColumns(g, db, tab, tp, info);
#endif   // XML_SUPPORT
	else
    sprintf(g->Message, "Usupported file type %s", ftype);

  return qrp;
} // end of RESTColumns

/* -------------------------- Class RESTDEF -------------------------- */

/***********************************************************************/
/*  DefineAM: define specific AM block values.                         */
/***********************************************************************/
bool RESTDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
{
	char     filename[_MAX_PATH + 1];
  int      rc = 0, n;
	bool     curl = false, xt = trace(515);
	LPCSTR   ftype;
	XGETREST grf = GetRestFunction(g);

	if (!grf)
		curl = true;

#if defined(MARIADB)
  ftype = GetStringCatInfo(g, "Type", "JSON");
#else   // !MARIADB
  // OEM tables must specify the file type
  ftype = GetStringCatInfo(g, "Ftype", "JSON");
#endif  // !MARIADB

  if (xt)
    htrc("ftype = %s am = %s\n", ftype, SVP(am));

  n = (!stricmp(ftype, "JSON")) ? 1
#if defined(XML_SUPPORT)
    : (!stricmp(ftype, "XML"))  ? 2
#endif   // XML_SUPPORT
    : (!stricmp(ftype, "CSV"))  ? 3 : 0;

  if (n == 0) {
    htrc("DefineAM: Unsupported REST table type %s\n", ftype);
    sprintf(g->Message, "Unsupported REST table type %s", ftype);
    return true;
  } // endif n

  Http = GetStringCatInfo(g, "Http", NULL);
  Uri = GetStringCatInfo(g, "Uri", NULL);
  Fn = GetStringCatInfo(g, "Filename", NULL);

  //  We used the file name relative to recorded datapath
  PlugSetPath(filename, Fn, GetPath());

	curl = GetBoolCatInfo("Curl", curl);

  // Retrieve the file from the web and copy it locally
	if (curl) {
		rc = Xcurl(g, Http, Uri, filename);
		xtrc(515, "Return from Xcurl: rc=%d\n", rc);
	} else if (grf) {
		rc = grf(g->Message, xt, Http, Uri, filename);
		xtrc(515, "Return from restGetFile: rc=%d\n", rc);
	} else {
		strcpy(g->Message, "Cannot access to curl nor casablanca");
		rc = 1;
	}	// endif !grf

  if (rc)
    return true;
  else switch (n) {
    case 1: Tdp = new (g) JSONDEF; break;
#if defined(XML_SUPPORT)
		case 2: Tdp = new (g) XMLDEF;  break;
#endif   // XML_SUPPORT
    case 3: Tdp = new (g) CSVDEF;  break;
    default: Tdp = NULL;
  } // endswitch n

  // Do make the table/view definition
  if (Tdp && Tdp->Define(g, Cat, Name, Schema, "REST"))
    Tdp = NULL; // Error occured

  if (xt)
    htrc("Tdp defined\n", rc);

  // Return true in case of error
  return (Tdp == NULL);
} // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB RESTDEF::GetTable(PGLOBAL g, MODE m)
{
  if (trace(515))
    htrc("REST GetTable mode=%d\n", m);

  if (m != MODE_READ && m != MODE_READX && m != MODE_ANY) {
    strcpy(g->Message, "REST tables are currently read only");
    return NULL;
  } // endif m

  return Tdp->GetTable(g, m); // Leave file type do the job
} // end of GetTable

/* ---------------------- End of Class RESTDEF ----------------------- */
