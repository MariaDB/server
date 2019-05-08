/* Copyright (C) MariaDB Corporation Ab

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

/*************** Mycat CC Program Source Code File (.CC) ***************/
/* PROGRAM NAME: MYCAT                                                 */
/* -------------                                                       */
/*  Version 1.6                                                        */
/*                                                                     */
/*  Author: Olivier Bertrand                       2012 - 2018         */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the DB description related routines.              */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#define DONT_DEFINE_VOID
#include <my_global.h>

#if defined(UNIX)
#include <unistd.h>
#include <string.h>
#endif
#include "handler.h"
#undef  OFFSET

/***********************************************************************/
/*  Include application header files                                   */
/*                                                                     */
/*  global.h     is header containing all global declarations.         */
/*  plgdbsem.h   is header containing DB application declarations.     */
/*  tabdos.h     is header containing TDBDOS classes declarations.     */
/*  MYCAT.h      is header containing DB description declarations.     */
/***********************************************************************/
#if defined(UNIX)
#include "osutil.h"
#endif   // UNIX
#include "global.h"
#include "plgdbsem.h"
//#include "reldef.h"
#include "xtable.h"
#include "tabext.h"
#include "tabcol.h"
#include "filamtxt.h"
#include "tabdos.h"
#include "tabfmt.h"
#if defined(VCT_SUPPORT)
#include "tabvct.h"
#endif   // VCT_SUPPORT
#include "tabsys.h"
#if defined(__WIN__)
#include "tabmac.h"
#include "tabwmi.h"
#endif   // __WIN__
//#include "tabtbl.h"
#include "tabxcl.h"
#include "tabtbl.h"
#include "taboccur.h"
#include "tabmul.h"
#include "tabmysql.h"
#if defined(ODBC_SUPPORT)
#define NODBC
#include "tabodbc.h"
#endif   // ODBC_SUPPORT
#if defined(JAVA_SUPPORT)
#define NJDBC
#include "tabjdbc.h"
#endif   // JAVA_SUPPORT
#include "tabpivot.h"
#include "tabvir.h"
#include "tabjson.h"
#include "ha_connect.h"
#if defined(XML_SUPPORT)
#include "tabxml.h"
#endif   // XML_SUPPORT
#if defined(JAVA_SUPPORT) || defined(CMGO_SUPPORT)
#include "mongo.h"
#endif   // JAVA_SUPPORT || CMGO_SUPPORT
#if defined(ZIP_SUPPORT)
#include "tabzip.h"
#endif   // ZIP_SUPPORT
#include "mycat.h"

/***********************************************************************/
/*  Extern static variables.                                           */
/***********************************************************************/
#if defined(__WIN__)
extern "C" HINSTANCE s_hModule;           // Saved module handle
#endif  // !__WIN__

PQRYRES OEMColumns(PGLOBAL g, PTOS topt, char *tab, char *db, bool info);

/***********************************************************************/
/*  Get the plugin directory.                                          */
/***********************************************************************/
char *GetPluginDir(void)
{
  return opt_plugin_dir;
} // end of GetPluginDir

/***********************************************************************/
/*  Get a unique enum table type ID.                                   */
/***********************************************************************/
TABTYPE GetTypeID(const char *type)
  {
  return (!type) ? TAB_UNDEF                      
                 : (!stricmp(type, "DOS"))   ? TAB_DOS
                 : (!stricmp(type, "FIX"))   ? TAB_FIX
                 : (!stricmp(type, "BIN"))   ? TAB_BIN
	               : (!stricmp(type, "CSV"))   ? TAB_CSV
                 : (!stricmp(type, "FMT"))   ? TAB_FMT
                 : (!stricmp(type, "DBF"))   ? TAB_DBF
#if defined(XML_SUPPORT)
                 : (!stricmp(type, "XML"))   ? TAB_XML
#endif
                 : (!stricmp(type, "INI"))   ? TAB_INI
                 : (!stricmp(type, "VEC"))   ? TAB_VEC
#if defined(ODBC_SUPPORT)
                 : (!stricmp(type, "ODBC"))  ? TAB_ODBC
#endif
#if defined(JAVA_SUPPORT)
								 : (!stricmp(type, "JDBC"))  ? TAB_JDBC
#endif
#if defined(JAVA_SUPPORT) || defined(CMGO_SUPPORT)
		             : (!stricmp(type, "MONGO") && MongoEnabled()) ? TAB_MONGO
#endif
								 : (!stricmp(type, "MYSQL")) ? TAB_MYSQL
                 : (!stricmp(type, "MYPRX")) ? TAB_MYSQL
                 : (!stricmp(type, "DIR"))   ? TAB_DIR
#if defined(__WIN__)
	               : (!stricmp(type, "MAC"))   ? TAB_MAC
	               : (!stricmp(type, "WMI"))   ? TAB_WMI
#endif
	               : (!stricmp(type, "TBL"))   ? TAB_TBL
	               : (!stricmp(type, "XCOL"))  ? TAB_XCL
	               : (!stricmp(type, "OCCUR")) ? TAB_OCCUR
                 : (!stricmp(type, "CATLG")) ? TAB_PRX  // Legacy
                 : (!stricmp(type, "PROXY")) ? TAB_PRX
                 : (!stricmp(type, "PIVOT")) ? TAB_PIVOT
                 : (!stricmp(type, "VIR"))   ? TAB_VIR
                 : (!stricmp(type, "JSON"))  ? TAB_JSON
#if defined(ZIP_SUPPORT)
								 : (!stricmp(type, "ZIP"))   ? TAB_ZIP
#endif
		             : (!stricmp(type, "OEM"))   ? TAB_OEM : TAB_NIY;
  } // end of GetTypeID

/***********************************************************************/
/*  Return true for table types based on file.                         */
/***********************************************************************/
bool IsFileType(TABTYPE type)
  {
  bool isfile;

  switch (type) {                      
    case TAB_DOS:
    case TAB_FIX:
    case TAB_BIN:
	  case TAB_CSV:
    case TAB_FMT:
    case TAB_DBF:
    case TAB_XML:
    case TAB_INI:
    case TAB_VEC:
    case TAB_JSON:
//	case TAB_ZIP:
      isfile= true;
      break;
    default:
      isfile= false;
      break;
    } // endswitch type

  return isfile;
  } // end of IsFileType

/***********************************************************************/
/*  Return true for table types returning exact row count.             */
/***********************************************************************/
bool IsExactType(TABTYPE type)
  {
  bool exact;

  switch (type) {                      
    case TAB_FIX:
    case TAB_BIN:
    case TAB_DBF:
//  case TAB_XML:     depends on Multiple || Xpand || Coltype
//  case TAB_JSON:    depends on Multiple || Xpand || Coltype
    case TAB_VEC:
    case TAB_VIR:
      exact= true;
      break;
    default:
      exact= false;
      break;
    } // endswitch type

  return exact;
  } // end of IsExactType

/***********************************************************************/
/*  Return true for table types accepting null fields.                 */
/***********************************************************************/
bool IsTypeNullable(TABTYPE type)
  {
  bool nullable;

  switch (type) {                      
    case TAB_MAC:
    case TAB_DIR:
      nullable= false;
      break;
    default:
      nullable= true;
      break;
    } // endswitch type

  return nullable;
  } // end of IsTypeNullable

/***********************************************************************/
/*  Return true for fixed record length tables.                        */
/***********************************************************************/
bool IsTypeFixed(TABTYPE type)
  {
  bool fix;

  switch (type) {                      
    case TAB_FIX:
    case TAB_BIN:
    case TAB_VEC:
//  case TAB_DBF:         ???
      fix= true;
      break;
    default:
      fix= false;
      break;
    } // endswitch type

  return fix;
  } // end of IsTypeFixed

/***********************************************************************/
/*  Return true for table indexable by XINDEX.                         */
/***********************************************************************/
bool IsTypeIndexable(TABTYPE type)
  {
  bool idx;

  switch (type) {                      
    case TAB_DOS:
    case TAB_CSV:
    case TAB_FMT:
    case TAB_FIX:
    case TAB_BIN:
    case TAB_VEC:
    case TAB_DBF:
    case TAB_JSON:
      idx= true;
      break;
    default:
      idx= false;
      break;
    } // endswitch type

  return idx;
  } // end of IsTypeIndexable

/***********************************************************************/
/*  Return index type: 0 NO, 1 XINDEX, 2 REMOTE.                       */
/***********************************************************************/
int GetIndexType(TABTYPE type)
  {
  int xtyp;

  switch (type) {                      
    case TAB_DOS:
    case TAB_CSV:
    case TAB_FMT:
    case TAB_FIX:
    case TAB_BIN:
    case TAB_VEC:
    case TAB_DBF:
    case TAB_JSON:
      xtyp= 1;
      break;
    case TAB_MYSQL:
    case TAB_ODBC:
		case TAB_JDBC:
		case TAB_MONGO:
			xtyp= 2;
      break;
    case TAB_VIR:
      xtyp= 3;
      break;
    default:
      xtyp= 0;
      break;
    } // endswitch type

  return xtyp;
  } // end of GetIndexType

/***********************************************************************/
/*  Get a unique enum catalog function ID.                             */
/***********************************************************************/
uint GetFuncID(const char *func)
  {
  uint fnc;

  if (!func)
    fnc= FNC_NO;
  else if (!strnicmp(func, "col", 3))
    fnc= FNC_COL;
  else if (!strnicmp(func, "tab", 3))
    fnc= FNC_TABLE;
  else if (!stricmp(func, "dsn") ||
           !strnicmp(func, "datasource", 10) ||
           !strnicmp(func, "source", 6) ||
           !strnicmp(func, "sqldatasource", 13))
    fnc= FNC_DSN;
  else if (!strnicmp(func, "driver", 6) ||
           !strnicmp(func, "sqldriver", 9))
    fnc= FNC_DRIVER;
  else
    fnc= FNC_NIY;

  return fnc;
  } // end of GetFuncID

/***********************************************************************/
/*  OEMColumn: Get table column info for an OEM table.                 */
/***********************************************************************/
PQRYRES OEMColumns(PGLOBAL g, PTOS topt, char *tab, char *db, bool info)
  {
  typedef PQRYRES (__stdcall *XCOLDEF) (PGLOBAL, void*, char*, char*, bool);
  const char *module, *subtype;
  char    c, soname[_MAX_PATH], getname[40] = "Col";
#if defined(__WIN__)
  HANDLE  hdll;               /* Handle to the external DLL            */
#else   // !__WIN__
  void   *hdll;               /* Handle for the loaded shared library  */
#endif  // !__WIN__
  XCOLDEF coldef = NULL;
  PQRYRES qrp = NULL;

  module = topt->module;
  subtype = topt->subtype;

  if (!module || !subtype)
    return NULL;

  /*********************************************************************/
  /*  Ensure that the .dll doesn't have a path.                        */
  /*  This is done to ensure that only approved dll from the system    */
  /*  directories are used (to make this even remotely secure).        */
  /*********************************************************************/
  if (check_valid_path(module, strlen(module))) {
    strcpy(g->Message, "Module cannot contain a path");
    return NULL;
  } else
    PlugSetPath(soname, module, GetPluginDir());
    
  // The exported name is always in uppercase
  for (int i = 0; ; i++) {
    c = subtype[i];
    getname[i + 3] = toupper(c);
    if (!c) break;
    } // endfor i

#if defined(__WIN__)
  // Load the Dll implementing the table
  if (!(hdll = LoadLibrary(soname))) {
    char  buf[256];
    DWORD rc = GetLastError();

    sprintf(g->Message, MSG(DLL_LOAD_ERROR), rc, soname);
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS, NULL, rc, 0,
                  (LPTSTR)buf, sizeof(buf), NULL);
    strcat(strcat(g->Message, ": "), buf);
    return NULL;
    } // endif hDll

  // Get the function returning an instance of the external DEF class
  if (!(coldef = (XCOLDEF)GetProcAddress((HINSTANCE)hdll, getname))) {
    sprintf(g->Message, MSG(PROCADD_ERROR), GetLastError(), getname);
    FreeLibrary((HMODULE)hdll);
    return NULL;
    } // endif coldef
#else   // !__WIN__
  const char *error = NULL;

  // Load the desired shared library
  if (!(hdll = dlopen(soname, RTLD_LAZY))) {
    error = dlerror();
    sprintf(g->Message, MSG(SHARED_LIB_ERR), soname, SVP(error));
    return NULL;
    } // endif Hdll

  // Get the function returning an instance of the external DEF class
  if (!(coldef = (XCOLDEF)dlsym(hdll, getname))) {
    error = dlerror();
    sprintf(g->Message, MSG(GET_FUNC_ERR), getname, SVP(error));
    dlclose(hdll);
    return NULL;
    } // endif coldef
#endif  // !__WIN__

  // Just in case the external Get function does not set error messages
  sprintf(g->Message, "Error getting column info from %s", subtype);

  // Get the table column definition
  qrp = coldef(g, topt, tab, db, info);

#if defined(__WIN__)
  FreeLibrary((HMODULE)hdll);
#else   // !__WIN__
  dlclose(hdll);
#endif  // !__WIN__

  return qrp;
  } // end of OEMColumns

/* ------------------------- Class CATALOG --------------------------- */

/***********************************************************************/
/*  CATALOG Constructor.                                               */
/***********************************************************************/
CATALOG::CATALOG(void)
  {
#if defined(__WIN__)
//DataPath= ".\\";
#else   // !__WIN__
//DataPath= "./";
#endif  // !__WIN__
  memset(&Ctb, 0, sizeof(CURTAB));
  Cbuf= NULL;
  Cblen= 0;
	DefHuge= false;
  } // end of CATALOG constructor

/* -------------------------- Class MYCAT ---------------------------- */

/***********************************************************************/
/*  MYCAT Constructor.                                                 */
/***********************************************************************/
MYCAT::MYCAT(PHC hc) : CATALOG()
  {
	Hc= hc;
  DefHuge= false;
  } // end of MYCAT constructor

/***********************************************************************/
/*  Nothing to do for CONNECT.                                         */
/***********************************************************************/
void MYCAT::Reset(void)
  {
  } // end of Reset

/***********************************************************************/
/*  GetTableDesc: retrieve a table descriptor.                         */
/*  Look for a table descriptor matching the name and type.            */
/***********************************************************************/
PRELDEF MYCAT::GetTableDesc(PGLOBAL g, PTABLE tablep,
                                       LPCSTR type, PRELDEF *)
  {
	if (trace(1))
		printf("GetTableDesc: name=%s am=%s\n", tablep->GetName(), SVP(type));

 	// If not specified get the type of this table
  if (!type)
    type= Hc->GetStringOption("Type","*");

  return MakeTableDesc(g, tablep, type);
  } // end of GetTableDesc

/***********************************************************************/
/*  MakeTableDesc: make a table/view description.                      */
/*  Note: caller must check if name already exists before calling it.  */
/***********************************************************************/
PRELDEF MYCAT::MakeTableDesc(PGLOBAL g, PTABLE tablep, LPCSTR am)
  {
  TABTYPE tc;
	LPCSTR  name = (PSZ)PlugDup(g, tablep->GetName());
	LPCSTR  schema = (PSZ)PlugDup(g, tablep->GetSchema());
  PRELDEF tdp= NULL;

	if (trace(1))
		printf("MakeTableDesc: name=%s schema=%s am=%s\n",
		                       name, SVP(schema), SVP(am));

  /*********************************************************************/
  /*  Get a unique enum identifier for types.                          */
  /*********************************************************************/
  tc= GetTypeID(am);

  switch (tc) {
    case TAB_FIX:
    case TAB_BIN:
    case TAB_DBF:
    case TAB_DOS: tdp= new(g) DOSDEF;   break;
    case TAB_CSV:
    case TAB_FMT: tdp= new(g) CSVDEF;   break;
    case TAB_INI: tdp= new(g) INIDEF;   break;
    case TAB_DIR: tdp= new(g) DIRDEF;   break;
#if defined(XML_SUPPORT)
    case TAB_XML: tdp= new(g) XMLDEF;   break;
#endif   // XML_SUPPORT
#if defined(VCT_SUPPORT)
		case TAB_VEC: tdp = new(g) VCTDEF;  break;
#endif   // VCT_SUPPORT
#if defined(ODBC_SUPPORT)
    case TAB_ODBC: tdp= new(g) ODBCDEF; break;
#endif   // ODBC_SUPPORT
#if defined(JAVA_SUPPORT)
		case TAB_JDBC: tdp= new(g) JDBCDEF; break;
#endif   // JAVA_SUPPORT
#if defined(__WIN__)
    case TAB_MAC: tdp= new(g) MACDEF;   break;
    case TAB_WMI: tdp= new(g) WMIDEF;   break;
#endif   // __WIN__
    case TAB_OEM: tdp= new(g) OEMDEF;   break;
	  case TAB_TBL: tdp= new(g) TBLDEF;   break;
	  case TAB_XCL: tdp= new(g) XCLDEF;   break;
	  case TAB_PRX: tdp= new(g) PRXDEF;   break;
		case TAB_OCCUR: tdp= new(g) OCCURDEF;	break;
		case TAB_MYSQL: tdp= new(g) MYSQLDEF;	break;
    case TAB_PIVOT: tdp= new(g) PIVOTDEF; break;
    case TAB_VIR: tdp= new(g) VIRDEF;   break;
    case TAB_JSON: tdp= new(g) JSONDEF; break;
#if defined(ZIP_SUPPORT)
		case TAB_ZIP: tdp = new(g) ZIPDEF;   break;
#endif   // ZIP_SUPPORT
#if defined(JAVA_SUPPORT) || defined(CMGO_SUPPORT)
		case TAB_MONGO:
			if (MongoEnabled()) {
				tdp = new(g) MGODEF;
				break;
			}	// endif enabled
			// fall through
#endif   // JAVA_SUPPORT || CMGO_SUPPORT
		default:
			sprintf(g->Message, MSG(BAD_TABLE_TYPE), am, name);
    } // endswitch

  // Do make the table/view definition
  if (tdp && tdp->Define(g, this, name, schema, am))
    tdp= NULL;

  return tdp;
  } // end of MakeTableDesc

/***********************************************************************/
/*  Initialize a Table Description Block construction.                 */
/***********************************************************************/
PTDB MYCAT::GetTable(PGLOBAL g, PTABLE tablep, MODE mode, LPCSTR type)
  {
  PRELDEF tdp;
  PTDB    tdbp= NULL;
//  LPCSTR  name= tablep->GetName();

	if (trace(1))
		printf("GetTableDB: name=%s\n", tablep->GetName());

  // Look for the description of the requested table
  tdp= GetTableDesc(g, tablep, type);

  if (tdp) {
		if (trace(1))
			printf("tdb=%p type=%s\n", tdp, tdp->GetType());

		if (tablep->GetSchema())
			tdp->Database = SetPath(g, tablep->GetSchema());
		
    tdbp= tdp->GetTable(g, mode);
		} // endif tdp

  if (tdbp) {
		if (trace(1))
			printf("tdbp=%p name=%s amtype=%d\n", tdbp, tdbp->GetName(),
																						tdbp->GetAmType());
    tablep->SetTo_Tdb(tdbp);
    tdbp->SetTable(tablep);
    tdbp->SetMode(mode);
    } // endif tdbp

  return (tdbp);
  } // end of GetTable

/***********************************************************************/
/*  ClearDB: Terminates Database usage.                                */
/***********************************************************************/
void MYCAT::ClearDB(PGLOBAL)
  {
  } // end of ClearDB

/* ------------------------ End of MYCAT --------------------------- */
