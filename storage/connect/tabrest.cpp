/*************** Rest C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: Rest   Version 1.5                                    */
/*  (C) Copyright to the author Olivier BERTRAND          2018 - 2019  */
/*  This program is the REST Web API support for MariaDB.              */
/*  When compiled without MARIADB defined, it is the EOM module code.  */
/***********************************************************************/

/***********************************************************************/
/*  Definitions needed by the included files.                          */
/***********************************************************************/
#if defined(MARIADB)
#include <my_global.h> // All MariaDB stuff
#else   // !MARIADB       OEM module
#include "mini-global.h"
#define _MAX_PATH 260
#if !defined(__WIN__)
#define __stdcall
#endif   // !__WIN__
#define _OS_H_INCLUDED     // Prevent os.h to be called
#endif  // !MARIADB

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  (x)table.h  is header containing the TDBASE declarations.          */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xtable.h"
#include "filamtxt.h"
#include "tabdos.h"
#include "plgxml.h"
#include "tabxml.h"
#include "tabjson.h"
#include "tabfmt.h"
#include "tabrest.h"

/***********************************************************************/
/*  Get the file from the Web.                                         */
/***********************************************************************/
int restGetFile(PGLOBAL g, PCSZ http, PCSZ uri, PCSZ fn);

#if defined(__WIN__)
static PCSZ slash = "\\";
#else // !__WIN__
static PCSZ slash = "/";
#define stricmp strcasecmp
#endif // !__WIN__

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
  strcat(strcat(strcat(strcpy(filename, "."), slash), db), slash);
  strncat(filename, fn, _MAX_PATH);

  // Retrieve the file from the web and copy it locally
  if (http && restGetFile(g, http, uri, filename)) {
    // sprintf(g->Message, "Failed to get file at %s", http);
  } else if (!stricmp(ftype, "XML"))
    qrp = XMLColumns(g, db, tab, tp, info);
  else if (!stricmp(ftype, "JSON"))
    qrp = JSONColumns(g, db, NULL, tp, info);
  else if (!stricmp(ftype, "CSV"))
    qrp = CSVColumns(g, NULL, tp, info);
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
  LPCSTR  ftype;

#if defined(MARIADB)
  ftype = GetStringCatInfo(g, "Type", "JSON");
#else   // !MARIADB
  // OEM tables must specify the file type
  ftype = GetStringCatInfo(g, "Ftype", "JSON");
#endif  // !MARIADB

  if (trace(515))
    htrc("ftype = %s am = %s\n", ftype, SVP(am));

  n = (!stricmp(ftype, "JSON")) ? 1
    : (!stricmp(ftype, "XML"))  ? 2
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
  strncat(strcpy(filename, GetPath()), Fn, _MAX_PATH);

  // Retrieve the file from the web and copy it locally
  rc = restGetFile(g, Http, Uri, filename);

  if (trace(515))
    htrc("Return from restGetFile: rc=%d\n", rc);

  if (rc)
    return true;
  else switch (n) {
    case 1: Tdp = new (g) JSONDEF; break;
    case 2: Tdp = new (g) XMLDEF;  break;
    case 3: Tdp = new (g) CSVDEF;  break;
    default: Tdp = NULL;
  } // endswitch n

  // Do make the table/view definition
  if (Tdp && Tdp->Define(g, Cat, Name, Schema, "REST"))
    Tdp = NULL; // Error occured

  if (trace(515))
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
