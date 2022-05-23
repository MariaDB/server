/************** tabrest C++ Program Source Code File (.CPP) ************/
/* PROGRAM NAME: tabrest   Version 2.1                                 */
/*  (C) Copyright to the author Olivier BERTRAND          2018 - 2021  */
/*  This program is the REST Web API support for MariaDB.              */
/*  The way Connect handles NOSQL data returned by REST queries is     */
/*  just by retrieving it as a file and then leave the existing data   */
/*  type tables (JSON, XML or CSV) process it as usual.                */
/***********************************************************************/

/***********************************************************************/
/*  Definitions needed by the included files.                          */
/***********************************************************************/
#include <my_global.h>    // All MariaDB stuff
#include <mysqld.h>
#include <sql_error.h>
#if !defined(_WIN32) && !defined(_WINDOWS)
#include <sys/types.h>
#include <sys/wait.h>
#endif	 // !_WIN32 && !_WINDOWS

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
#define PUSH_WARNING(M) push_warning(current_thd, Sql_condition::WARN_LEVEL_NOTE, 0, M)
#else
#define PUSH_WARNING(M) htrc(M)
#endif

static XGETREST getRestFnc = NULL;
static int Xcurl(PGLOBAL g, PCSZ Http, PCSZ Uri, PCSZ filename);

/***********************************************************************/
/*  Xcurl: retrieve the REST answer by executing cURL.                 */
/***********************************************************************/
int Xcurl(PGLOBAL g, PCSZ Http, PCSZ Uri, PCSZ filename)
{
	char  buf[512];
	int   rc = 0;

	if (strchr(filename, '"')) {
		strcpy(g->Message, "Invalid file name");
		return 1;
	} // endif filename

	if (Uri) {
		if (*Uri == '/' || Http[strlen(Http) - 1] == '/')
			my_snprintf(buf, sizeof(buf)-1, "%s%s", Http, Uri);
		else
			my_snprintf(buf, sizeof(buf)-1, "%s/%s", Http, Uri);

	} else
		my_snprintf(buf, sizeof(buf)-1, "%s", Http);

#if defined(_WIN32)
	char cmd[1024];
	STARTUPINFO si;
	PROCESS_INFORMATION pi;

	sprintf(cmd, "curl \"%s\" -o \"%s\"", buf, filename);

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	// Start the child process. 
	if (CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		// Wait until child process exits.
		WaitForSingleObject(pi.hProcess, INFINITE);

		// Close process and thread handles. 
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	} else {
		sprintf(g->Message, "CreateProcess curl failed (%d)", GetLastError());
		rc = 1;
	}	// endif CreateProcess
#else   // !_WIN32
	char  fn[600];
	pid_t pID;

	// Check if curl package is availabe by executing subprocess
	FILE *f= popen("command -v curl", "r");

	if (!f) {
			strcpy(g->Message, "Problem in allocating memory.");
			return 1;
	} else {
		char   temp_buff[50];
		size_t len = fread(temp_buff,1, 50, f);

		if(!len) {
			strcpy(g->Message, "Curl not installed.");
			return 1;
		}	else
			pclose(f);

	} // endif f
	
#ifdef HAVE_VFORK
       pID = vfork();
#else
       pID = fork();
#endif
	sprintf(fn, "-o%s", filename);

	if (pID == 0) {
		// Code executed by child process
		execlp("curl", "curl", buf, fn, (char*)NULL);

		// If execlp() is successful, we should not reach this next line.
		strcpy(g->Message, "Unsuccessful execlp from vfork()");
		exit(1);
	} else if (pID < 0) {
		// failed to fork
		strcpy(g->Message, "Failed to fork");
		rc = 1;
	} else {
		// Parent process
		wait(NULL);  // Wait for the child to terminate
	}	// endif pID
#endif  // !_WIN32

	return rc;
} // end of Xcurl

/***********************************************************************/
/*  GetREST: load the Rest lib and get the Rest function.              */
/***********************************************************************/
XGETREST GetRestFunction(PGLOBAL g)
{
	if (getRestFnc)
		return getRestFnc;
	
#if !defined(REST_SOURCE)
	if (trace(515))
		htrc("Looking for GetRest library\n");

#if defined(_WIN32) || defined(_WINDOWS)
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
#else   // !_WIN32
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
#endif  // !_WIN32
#else   // REST_SOURCE
	getRestFnc = restGetFile;
#endif	// REST_SOURCE

	return getRestFnc;
} // end of GetRestFunction

/***********************************************************************/
/*  Return the columns definition to MariaDB.                          */
/***********************************************************************/
PQRYRES RESTColumns(PGLOBAL g, PTOS tp, char *tab, char *db, bool info)
{
  PQRYRES  qrp= NULL;
  char     filename[_MAX_PATH + 1];  // MAX PATH ???
	int      rc;
  PCSZ     http, uri, fn, ftype;
	XGETREST grf = NULL;
	bool     curl = GetBooleanTableOption(g, tp, "Curl", false);

	if (!curl && !(grf = GetRestFunction(g)))
		curl = true;

  http = GetStringTableOption(g, tp, "Http", NULL);
  uri = GetStringTableOption(g, tp, "Uri", NULL);
  ftype = GetStringTableOption(g, tp, "Type", "JSON");
	fn = GetStringTableOption(g, tp, "Filename", NULL);

	if (!fn) {
		int n, m = strlen(ftype) + 1;

		strcat(strcpy(filename, tab), ".");
		n = strlen(filename);

		// Fold ftype to lower case
		for (int i = 0; i < m; i++)
			filename[n + i] = tolower(ftype[i]);

		fn = filename;
		tp->subtype = PlugDup(g, fn);
		sprintf(g->Message, "No file name. Table will use %s", fn);
		PUSH_WARNING(g->Message);
	}	// endif fn

  //  We used the file name relative to recorded datapath
	PlugSetPath(filename, fn, db);
	remove(filename);

  // Retrieve the file from the web and copy it locally
	if (curl)
		rc = Xcurl(g, http, uri, filename);
	else
		rc = grf(g->Message, trace(515), http, uri, filename);

	if (rc) {
		strcpy(g->Message, "Cannot access to curl nor casablanca");
		return NULL;
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
	char     filename[_MAX_PATH + 1];
  int      rc = 0, n;
	bool     xt = trace(515);
	LPCSTR   ftype;
	XGETREST grf = NULL;
	bool     curl = GetBoolCatInfo("Curl", false);

	if (!curl && !(grf = GetRestFunction(g)))
		curl = true;

  ftype = GetStringCatInfo(g, "Type", "JSON");

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
	remove(filename);

  // Retrieve the file from the web and copy it locally
	if (curl) {
		rc = Xcurl(g, Http, Uri, filename);
		xtrc(515, "Return from Xcurl: rc=%d\n", rc);
	} else {
		rc = grf(g->Message, xt, Http, Uri, filename);
		xtrc(515, "Return from restGetFile: rc=%d\n", rc);
	} // endelse

	if (rc) {
		// strcpy(g->Message, "Cannot access to curl nor casablanca");
		return true;
	} else switch (n) {
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
