/************** tabrest C++ Program Source Code File (.CPP) ************/
/* PROGRAM NAME: tabrest   Version 1.7                                 */
/*  (C) Copyright to the author Olivier BERTRAND          2018 - 2019  */
/*  This program is the REST Web API support for MariaDB.              */
/*  When compiled without MARIADB defined, it is the EOM module code.  */
/***********************************************************************/

/***********************************************************************/
/*  Definitions needed by the included files.                          */
/***********************************************************************/
#if defined(MARIADB)
#include <my_global.h>    // All MariaDB stuff
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

static XGETREST getRestFnc = NULL;

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
/*  GetREST: get the external TABDEF from OEM module.                  */
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
  PQRYRES qrp= NULL;
  char filename[_MAX_PATH + 1];  // MAX PATH ???
  PCSZ http, uri, fn, ftype;
	XGETREST grf = GetRestFunction(g);

	if (!grf)
		return NULL;

  http = GetStringTableOption(g, tp, "Http", NULL);
  uri = GetStringTableOption(g, tp, "Uri", NULL);
  fn = GetStringTableOption(g, tp, "Filename", "rest.json");
#if defined(MARIADB)
  ftype = GetStringTableOption(g, tp, "Type", "JSON");
#else   // !MARIADB
  // OEM tables must specify the file type
  ftype = GetStringTableOption(g, tp, "Ftype", "JSON");
#endif  // !MARIADB

  //  We used the file name relative to recorded datapath
  snprintf(filename, sizeof filename, IF_WIN(".\\%s\\%s","./%s/%s"), db, fn);

  // Retrieve the file from the web and copy it locally
	if (http && grf(g->Message, trace(515), http, uri, filename)) {
			// sprintf(g->Message, "Failed to get file at %s", http);
  } else if (!stricmp(ftype, "JSON"))
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
	char    filename[_MAX_PATH + 1];
  int     rc = 0, n;
	bool    xt = trace(515);
	LPCSTR  ftype;
	XGETREST grf = GetRestFunction(g);

	if (!grf)
		return true;

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
    htrc("DefineAM: Unsupported REST table type %s", am);
    sprintf(g->Message, "Unsupported REST table type %s", am);
    return true;
  } // endif n

  Http = GetStringCatInfo(g, "Http", NULL);
  Uri = GetStringCatInfo(g, "Uri", NULL);
  Fn = GetStringCatInfo(g, "Filename", "rest.json");

  //  We used the file name relative to recorded datapath
  //PlugSetPath(filename, Fn, GetPath());
  strcpy(filename, GetPath());
	strncat(filename, Fn, _MAX_PATH - strlen(filename));

  // Retrieve the file from the web and copy it locally
	rc = grf(g->Message, xt, Http, Uri, filename);

  if (xt)
    htrc("Return from restGetFile: rc=%d\n", rc);

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

  if (m != MODE_READ && m != MODE_READX) {
    strcpy(g->Message, "REST tables are currently read only");
    return NULL;
  } // endif m

  return Tdp->GetTable(g, m); // Leave file type do the job
} // end of GetTable

/* ---------------------- End of Class RESTDEF ----------------------- */
