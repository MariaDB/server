/*************** Rest C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: Rest   Version 1.3                                    */
/*  (C) Copyright to the author Olivier BERTRAND          2018 - 2019  */
/*  This program is the REST OEM (Web API support) module definition.  */
/***********************************************************************/

/***********************************************************************/
/*  Definitions needed by the included files.                          */
/***********************************************************************/
#include <my_global.h> // All MariaDB stuff

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
#include "plgxml.h"
#include "tabdos.h"
#include "tabfmt.h"
#include "tabjson.h"
#include "tabrest.h"
#include "tabxml.h"

/***********************************************************************/
/*  Get the file from the Web.                                         */
/***********************************************************************/
int restGetFile(PGLOBAL g, PCSZ http, PCSZ uri, PCSZ fn);

#if defined(__WIN__)
static PCSZ slash= "\\";
#else // !__WIN__
static PCSZ slash= "/";
#define stricmp strcasecmp
#endif // !__WIN__

/***********************************************************************/
/*  Return the columns definition to MariaDB.                          */
/***********************************************************************/
PQRYRES RESTColumns(PGLOBAL g, PTOS tp, char *tab, char *db, bool info)
{
  PQRYRES qrp= NULL;
  char filename[_MAX_PATH];
  PCSZ http, uri, fn, ftype;

  http= GetStringTableOption(g, tp, "Http", NULL);
  uri= GetStringTableOption(g, tp, "Uri", NULL);
  fn= GetStringTableOption(g, tp, "Filename", "rest.json");
	ftype = GetStringTableOption(g, tp, "Type", "JSON");

  //  We used the file name relative to recorded datapath
  strcat(strcat(strcat(strcpy(filename, "."), slash), db), slash);
  strncat(filename, fn, _MAX_PATH);

  // Retrieve the file from the web and copy it locally
  if (http && restGetFile(g, http, uri, filename)) {
    // sprintf(g->Message, "Failed to get file at %s", http);
  } else if (!stricmp(ftype, "XML"))
    qrp= XMLColumns(g, db, tab, tp, info);
  else if (!stricmp(ftype, "JSON"))
    qrp= JSONColumns(g, db, NULL, tp, info);
  else if (!stricmp(ftype, "CSV"))
    qrp= CSVColumns(g, NULL, tp, info);
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
  char    filename[_MAX_PATH];
	TABTYPE type= GetTypeID(am);

	switch (type) {
	case TAB_JSON:
	case TAB_XML:
	case TAB_CSV:
		break;
	default:
		sprintf(g->Message, "Unsupported REST table type %s", am);
		return true;
	} // endswitch type

  Http= GetStringCatInfo(g, "Http", NULL);
  Uri= GetStringCatInfo(g, "Uri", NULL);
  Fn= GetStringCatInfo(g, "Filename", "rest.json");

  //  We used the file name relative to recorded datapath
  PlugSetPath(filename, Fn, GetPath());

  // Retrieve the file from the web and copy it locally
  if (Http && restGetFile(g, Http, Uri, filename)) {}
  else if (type == TAB_JSON)
    Tdp= new (g) JSONDEF;
  else if (type == TAB_XML)
    Tdp= new (g) XMLDEF;
  else if (type == TAB_CSV)
    Tdp= new (g) CSVDEF;
  else
    sprintf(g->Message, "Unsupported REST table type %s", am);

  // Do make the table/view definition
  if (Tdp && Tdp->Define(g, Cat, Name, Schema, "REST"))
    Tdp= NULL; // Error occured

  // Return true in case of error
  return (Tdp == NULL);
} // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB RESTDEF::GetTable(PGLOBAL g, MODE m)
{
	if (m != MODE_READ && m != MODE_READX) {
		strcpy(g->Message, "REST tables are currently read only");
		return NULL;
	} // endif m

  return Tdp->GetTable(g, m); // Leave file type do the job
} // end of GetTable

/* ---------------------- End of Class RESTDEF ----------------------- */
