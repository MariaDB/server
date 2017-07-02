/************** mongo C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: mongo     Version 1.0                                 */
/*  (C) Copyright to the author Olivier BERTRAND          2017         */
/*  These programs are the MGODEF class execution routines.            */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the MariaDB header file.              */
/***********************************************************************/
#include <my_global.h>

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xtable.h"
#include "tabext.h"
#if defined(MONGO_SUPPORT)
#include "tabmgo.h"
#endif   // MONGO_SUPPORT
#if defined(JDBC_SUPPORT)
#include "tabjmg.h"
#endif   // JDBC_SUPPORT

/* -------------------------- Class MGODEF --------------------------- */

MGODEF::MGODEF(void)
{
	Driver = NULL;
	Uri = NULL;
	Colist = NULL;
	Filter = NULL;
	Level = 0;
	Base = 0;
	Version = 0;
	Pipe = false;
} // end of MGODEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values.                         */
/***********************************************************************/
bool MGODEF::DefineAM(PGLOBAL g, LPCSTR, int poff)
{
	if (EXTDEF::DefineAM(g, "MGO", poff))
		return true;
	else if (!Tabschema)
		Tabschema = GetStringCatInfo(g, "Dbname", "*");

# if !defined(JDBC_SUPPORT)
	Driver = "C";
#elif !defined(MONGO_SUPPORT)
	Driver = "JAVA";
#else
	Driver = GetStringCatInfo(g, "Driver", "C");
#endif
	Uri = GetStringCatInfo(g, "Connect", "mongodb://localhost:27017");
	Colist = GetStringCatInfo(g, "Colist", NULL);
	Filter = GetStringCatInfo(g, "Filter", NULL);
	Base = GetIntCatInfo("Base", 0) ? 1 : 0;
	Version = GetIntCatInfo("Version", 3);

	if (Version == 2)
		Wrapname = GetStringCatInfo(g, "Wrapper", "Mongo2Interface");
	else
		Wrapname = GetStringCatInfo(g, "Wrapper", "Mongo3Interface");

	Pipe = GetBoolCatInfo("Pipeline", false);
	return false;
} // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB MGODEF::GetTable(PGLOBAL g, MODE m)
{
	if (Catfunc == FNC_COL) {
#if defined(MONGO_SUPPORT)
		if (Driver && toupper(*Driver) == 'C')
			return new(g)TDBGOL(this);
#endif   // MONGO_SUPPORT
		strcpy(g->Message, "No column find for Java Mongo yet");
		return NULL;
	} // endif Catfunc

#if defined(MONGO_SUPPORT)
	if (Driver && toupper(*Driver) == 'C')
		return new(g) TDBMGO(this);
#endif   // MONGO_SUPPORT
#if defined(JDBC_SUPPORT)
	return new(g) TDBJMG(this);
#else   // !JDBC_SUPPORT
	strcpy(g->Message, "No MONGO nor Java support");
	return NULL;
#endif  // !JDBC_SUPPORT
} // end of GetTable

