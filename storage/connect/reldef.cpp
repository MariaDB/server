/************* RelDef CPP Program Source Code File (.CPP) **************/
/* PROGRAM NAME: RELDEF                                                */
/* -------------                                                       */
/*  Version 1.7                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2004-2019    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the DB definition related routines.               */
/*                                                                     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include "my_global.h"
#if defined(_WIN32)
#include <sqlext.h>
#else
//#include <dlfcn.h>          // dlopen(), dlclose(), dlsym() ...
#include "osutil.h"
//#include "sqlext.h"
#endif
#include "handler.h"

/***********************************************************************/
/*  Include application header files                                   */
/*                                                                     */
/*  global.h     is header containing all global declarations.         */
/*  plgdbsem.h   is header containing DB application declarations.     */
/*  catalog.h    is header containing DB description declarations.     */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "reldef.h"
#include "colblk.h"
#include "tabcol.h"
#include "filamap.h"
#include "filamfix.h"
#if defined(VCT_SUPPORT)
#include "filamvct.h"
#endif   // VCT_SUPPORT
#if defined(GZ_SUPPORT)
#include "filamgz.h"
#endif   // GZ_SUPPORT
#include "tabdos.h"
#include "valblk.h"
#include "tabmul.h"
#include "ha_connect.h"
#include "mycat.h"

#if !defined(_WIN32)
extern handlerton *connect_hton;
#endif   // !_WIN32

/***********************************************************************/
/*  External function.                                                 */
/***********************************************************************/
USETEMP UseTemp(void);
char   *GetPluginDir(void);
PQRYRES OEMColumns(PGLOBAL g, PTOS topt, char* tab, char* db, bool info);

/***********************************************************************/
/*  OEMColumns: Get table column info for an OEM table.                */
/***********************************************************************/
PQRYRES OEMColumns(PGLOBAL g, PTOS topt, char* tab, char* db, bool info)
{
	typedef PQRYRES(__stdcall* XCOLDEF) (PGLOBAL, void*, char*, char*, bool);
	const char* module, * subtype;
	char    c, soname[_MAX_PATH], getname[40] = "Col";
#if defined(_WIN32)
	HANDLE  hdll;               /* Handle to the external DLL            */
#else   // !_WIN32
	void* hdll;               /* Handle for the loaded shared library  */
#endif  // !_WIN32
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
	}
	else if (strlen(subtype)+1+3 >= sizeof(getname)) {
		strcpy(g->Message, "Subtype string too long");
		return NULL;
	}
	else
		PlugSetPath(soname, module, GetPluginDir());

	// The exported name is always in uppercase
	for (int i = 0; ; i++) {
		c = subtype[i];
		getname[i + 3] = toupper(c);
		if (!c) break;
	} // endfor i

#if defined(_WIN32)
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
#else   // !_WIN32
	const char* error = NULL;

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
#endif  // !_WIN32

	// Just in case the external Get function does not set error messages
	sprintf(g->Message, "Error getting column info from %s", subtype);

	// Get the table column definition
	qrp = coldef(g, topt, tab, db, info);

#if defined(_WIN32)
	FreeLibrary((HMODULE)hdll);
#else   // !_WIN32
	dlclose(hdll);
#endif  // !_WIN32

	return qrp;
} // end of OEMColumns

/* --------------------------- Class RELDEF -------------------------- */

/***********************************************************************/
/*  RELDEF Constructor.                                                */
/***********************************************************************/
RELDEF::RELDEF(void)
  {
  Next = NULL;
  To_Cols = NULL;
  Name = NULL;
  Database = NULL;
  Cat = NULL;
  Hc = NULL;
  } // end of RELDEF constructor

/***********************************************************************/
/*  This function return a pointer to the Table Option Struct.         */
/***********************************************************************/
PTOS RELDEF::GetTopt(void)
  {
  return Hc->GetTableOptionStruct();
  } // end of GetTopt

/***********************************************************************/
/*  This function sets an integer table information.                   */
/***********************************************************************/
bool RELDEF::SetIntCatInfo(PCSZ what, int n)
  {
  return Hc->SetIntegerOption(what, n);
  } // end of SetIntCatInfo

/***********************************************************************/
/*  This function returns integer table information.                   */
/***********************************************************************/
int RELDEF::GetIntCatInfo(PCSZ what, int idef)
  {
  int n= Hc->GetIntegerOption(what);

  return (n == NO_IVAL) ? idef : n;
  } // end of GetIntCatInfo

/***********************************************************************/
/*  This function returns Boolean table information.                   */
/***********************************************************************/
bool RELDEF::GetBoolCatInfo(PCSZ what, bool bdef)
  {
  bool b= Hc->GetBooleanOption(what, bdef);

  return b;
  } // end of GetBoolCatInfo

/***********************************************************************/
/*  This function returns size catalog information.                    */
/***********************************************************************/
int RELDEF::GetSizeCatInfo(PCSZ what, PCSZ sdef)
  {
  char c;
  PCSZ s;
  int  i, n= 0;

  if (!(s= Hc->GetStringOption(what)))
    s= sdef;

  if ((i= sscanf(s, " %d %c ", &n, &c)) == 2)
    switch (toupper(c)) {
      case 'M':
        n *= 1024;
        // fall through
      case 'K':
        n *= 1024;
      } // endswitch c

  return n;
} // end of GetSizeCatInfo

/***********************************************************************/
/*  This function sets char table information in buf.                  */
/***********************************************************************/
int RELDEF::GetCharCatInfo(PCSZ what, PCSZ sdef, char *buf, int size)
  {
  PCSZ s= Hc->GetStringOption(what);

  strncpy(buf, ((s) ? s : sdef), size);
  return size;
  } // end of GetCharCatInfo

/***********************************************************************/
/*  To be used by any TDB's.                                           */
/***********************************************************************/
bool RELDEF::Partitioned(void)
  {
  return Hc->IsPartitioned();
  } // end of Partitioned

/***********************************************************************/
/*  This function returns string table information.                    */
/*  Default parameter is "*" to get the handler default.               */
/***********************************************************************/
char *RELDEF::GetStringCatInfo(PGLOBAL g, PCSZ what, PCSZ sdef)
  {
  char *sval = NULL;
  PCSZ  name, s= Hc->GetStringOption(what, sdef);

  if (s) {
    if (!Hc->IsPartitioned() ||
        (stricmp(what, "filename") && stricmp(what, "tabname")
                                   && stricmp(what, "connect")))
      sval= PlugDup(g, s);
    else
      sval= (char*)s;

  } else if (!stricmp(what, "filename")) {
    // Return default file name
    PCSZ ftype= Hc->GetStringOption("Type", "*");
    int  i, n;

    if (IsFileType(GetTypeID(ftype))) {
      name= Hc->GetPartName();
      sval= (char*)PlugSubAlloc(g, NULL, strlen(name) + 12);
      strcat(strcpy(sval, name), ".");
      n= strlen(sval);

      // Fold ftype to lower case
      for (i= 0; i < 12; i++)
        if (!ftype[i]) {
          sval[n+i]= 0;
          break;
        } else
          sval[n+i]= tolower(ftype[i]);

      } // endif FileType

  } // endif s

  return sval;
  } // end of GetStringCatInfo

/* --------------------------- Class TABDEF -------------------------- */

/***********************************************************************/
/*  TABDEF Constructor.                                                */
/***********************************************************************/
TABDEF::TABDEF(void)
  {
  Schema = NULL;
  Desc = NULL;
	Recfm = RECFM_DFLT;
  Catfunc = FNC_NO;
  Card = 0;
  Elemt = 0;
  Sort = 0;
  Multiple = 0;
  Degree = 0;
  Pseudo = 0;
  Read_Only = false;
  m_data_charset = NULL;
  csname = NULL;
  } // end of TABDEF constructor

/***********************************************************************/
/*  Return the table format.                                           */
/***********************************************************************/
RECFM TABDEF::GetTableFormat(const char* type)
{
	RECFM recfm = Recfm;

	if (Catfunc != FNC_NO)
		recfm = RECFM_NAF;
	else if (recfm == RECFM_DFLT)
		// Default format depends on the table type
		switch (GetTypeID(type)) {
		case TAB_DOS: recfm = RECFM_VAR; break;
		case TAB_CSV: recfm = RECFM_CSV; break;
		case TAB_FMT: recfm = RECFM_FMT; break;
		case TAB_FIX: recfm = RECFM_FIX; break;
		case TAB_BIN: recfm = RECFM_BIN; break;
		case TAB_VEC: recfm = RECFM_VCT; break;
		case TAB_DBF: recfm = RECFM_DBF; break;
		case TAB_XML: recfm = RECFM_XML; break;
		case TAB_DIR: recfm = RECFM_DIR; break;
		default:			recfm = RECFM_NAF; break;
		} // endswitch type

	return recfm;
} // end of GetTableFormat

/***********************************************************************/
/*  Define: initialize the table definition block from XDB file.       */
/***********************************************************************/
bool TABDEF::Define(PGLOBAL g, PCATLG cat,
                    LPCSTR name, LPCSTR schema, LPCSTR am)
{
  int   poff = 0;

  Hc = ((MYCAT*)cat)->GetHandler();
  Name = (PSZ)name;
  Schema = (PSZ)Hc->GetDBName(schema);
  Cat = cat;
  Catfunc = GetFuncID(GetStringCatInfo(g, "Catfunc", NULL));
  Elemt = GetIntCatInfo("Elements", 0);
  Multiple = GetIntCatInfo("Multiple", 0);
  Degree = GetIntCatInfo("Degree", 0);
  Read_Only = GetBoolCatInfo("ReadOnly", false);
  const char *data_charset_name= GetStringCatInfo(g, "Data_charset", NULL);
  m_data_charset= data_charset_name ?
                  get_charset_by_csname(data_charset_name, MY_CS_PRIMARY, 0):
                  NULL;
  csname = GetStringCatInfo(g, "Table_charset", NULL);

	// Do the definition of AM specific fields
	if (DefineAM(g, am, 0))
		return true;

	// Get The column definitions
	if (stricmp(am, "OEM") && GetColCatInfo(g) < 0)
		return true;

	Hc->tshp = NULL;    // TO BE CHECKED
	return false;
} // end of Define

/***********************************************************************/
/*  This function returns the database data path.                      */
/***********************************************************************/
PCSZ TABDEF::GetPath(void)
  {
  return (Database) ? Database : (Hc) ? Hc->GetDataPath() : NULL;
  } // end of GetPath

/***********************************************************************/
/*  This function returns column table information.                    */
/***********************************************************************/
int TABDEF::GetColCatInfo(PGLOBAL g)
  {
  char    *type = GetStringCatInfo(g, "Type", "*");
  char     c, fty, eds;
  int      i, n, loff, poff, nof, nlg;
  void    *field = NULL;
  RECFM    trf;
  PCOLDEF  cdp, lcdp = NULL, tocols= NULL;
  PCOLINFO pcf= (PCOLINFO)PlugSubAlloc(g, NULL, sizeof(COLINFO));

  memset(pcf, 0, sizeof(COLINFO));

  // Get the table format
	trf = GetTableFormat(type);

  // Take care of the column definitions
  i= poff= nof= nlg= 0;

#if defined(_WIN32)
  // Offsets of HTML and DIR tables start from 0, DBF at 1
  loff= (trf == RECFM_DBF) ? 1 : (trf  == RECFM_XML || trf  == RECFM_DIR) ? -1 : 0;
#else   // !_WIN32
  // Offsets of HTML tables start from 0, DIR and DBF at 1
  loff = (trf  == RECFM_DBF || trf  == RECFM_DIR) ? 1 : (trf  == RECFM_XML) ? -1 : 0;
#endif  // !_WIN32

  while (true) {
    // Default Offset depends on table format
    switch (trf ) {
      case RECFM_VAR:
      case RECFM_FIX:
      case RECFM_BIN:
      case RECFM_VCT:
      case RECFM_DBF:
        poff= loff + nof;        // Default next offset
        nlg= MY_MAX(nlg, poff);    // Default lrecl
        break;
      case RECFM_CSV:
      case RECFM_FMT:
        nlg+= nof;
      case RECFM_DIR:
      case RECFM_XML:
        poff= loff + (pcf->Flags & U_VIRTUAL ? 0 : 1);
        break;
      //case RECFM_INI:
      //case RECFM_MAC:
      //case RECFM_TBL:
      //case RECFM_XCL:
      //case RECFM_OCCUR:
      //case RECFM_PRX:
      case RECFM_OEM:
        poff = 0;      // Offset represents an independant flag
        break;
      default:         // PLG ODBC JDBC MYSQL WMI...
        poff = 0;      // NA
        break;
      } // endswitch trf 

//    do {
      field= Hc->GetColumnOption(g, field, pcf);
//    } while (field && (*pcf->Name =='*' /*|| pcf->Flags & U_VIRTUAL*/));

    if (trf  == RECFM_DBF && pcf->Type == TYPE_DATE && !pcf->Datefmt) {
      // DBF date format defaults to 'YYYMMDD'
      pcf->Datefmt= "YYYYMMDD";
      pcf->Length= 8;
      } // endif trf 

    if (!field)
      break;

    // Allocate the column description block
    cdp= new(g) COLDEF;

    if ((nof= cdp->Define(g, NULL, pcf, poff)) < 0)
      return -1;             // Error, probably unhandled type
    else
      loff= cdp->GetOffset();

    switch (trf ) {
      case RECFM_VCT:
        cdp->SetOffset(0);     // Not to have shift
      case RECFM_BIN:
        // BIN/VEC are packed by default
        if (nof) {
          // Field width is the internal representation width
          // that can also depend on the column format
          fty = cdp->Decode ? 'C' : 'X';
          eds = 0;
          n = 0;

          if (cdp->Fmt && !cdp->Decode) {
            for (i = 0; cdp->Fmt[i]; i++) {
              c = toupper(cdp->Fmt[i]);

              if (isdigit(c))
                n = (n * 10 + (c - '0'));
              else if (c == 'L' || c == 'B' || c == 'H')
                eds = c;
              else
                fty = c;

              } // endfor i

          } // endif Fmt

          if (n)
            nof = n;
          else switch (fty) {
            case 'X':
              if (eds && IsTypeChar(cdp->Buf_Type))
                nof = sizeof(longlong);
              else
                nof= cdp->Clen;

              break;
            case 'C':                         break;
            case 'R':
            case 'F': nof = sizeof(float);    break;
            case 'I': nof = sizeof(int);      break;
            case 'D': nof = sizeof(double);   break;
            case 'S': nof = sizeof(short);    break;
            case 'T': nof = sizeof(char);     break;
            case 'G': nof = sizeof(longlong); break;
            default:  /* Wrong format */
              sprintf(g->Message, "Invalid format %c", fty);
              return -1;
            } // endswitch fty

          } // endif nof

      default:
        break;
      } // endswitch trf 

    if (lcdp)
      lcdp->SetNext(cdp);
    else
      tocols= cdp;

    lcdp= cdp;
    i++;
    } // endwhile

  // Degree is the the number of defined columns (informational)
  if (i != GetDegree())
    SetDegree(i);

  if (GetDefType() == TYPE_AM_DOS) {
    int     ending, recln= 0;

		ending = Hc->GetIntegerOption("Ending");

    // Calculate the default record size
    switch (trf ) {
      case RECFM_FIX:
      case RECFM_BIN:
        recln= nlg + ending;     // + length of line ending
        break;
      case RECFM_VCT:
        recln= nlg;

//      if ((k= (pak < 0) ? 8 : pak) > 1)
          // See above for detailed comment
          // Round up lrecl to multiple of 8 or pak
//        recln= ((recln + k - 1) / k) * k;

        break;
      case RECFM_VAR:
      case RECFM_DBF:
        recln= nlg;
        break;
      case RECFM_CSV:
      case RECFM_FMT:
        // The number of separators (assuming an extra one can exist)
//      recln= poff * ((qotd) ? 3 : 1);  to be investigated
        recln= nlg + poff * 3;     // To be safe
      default:
        break;
      } // endswitch trf 

    // lrecl must be at least recln to avoid buffer overflow
    if (trace(1))
      htrc("Lrecl: Calculated=%d defined=%d\n",
        recln, Hc->GetIntegerOption("Lrecl"));

    recln = MY_MAX(recln, Hc->GetIntegerOption("Lrecl"));
    Hc->SetIntegerOption("Lrecl", recln);
    ((PDOSDEF)this)->SetLrecl(recln);

    if (trace(1))
      htrc("Lrecl set to %d\n", recln);

    } // endif TYPE

  // Attach the column definition to the tabdef
  SetCols(tocols);
  return poff;
  } // end of GetColCatInfo

/***********************************************************************/
/*  SetIndexInfo: retrieve index description from the table structure. */
/***********************************************************************/
void TABDEF::SetIndexInfo(void)
  {
  // Attach new index(es)
  SetIndx(Hc->GetIndexInfo());
  } // end of SetIndexInfo

/* --------------------------- Class OEMDEF -------------------------- */

/***********************************************************************/
/*  GetXdef: get the external TABDEF from OEM module.                  */
/***********************************************************************/
PTABDEF OEMDEF::GetXdef(PGLOBAL g)
  {
  typedef PTABDEF (__stdcall *XGETDEF) (PGLOBAL, void *);
  char    c, soname[_MAX_PATH], getname[40] = "Get";
  PTABDEF xdefp;
  XGETDEF getdef = NULL;
  PCATLG  cat = Cat;

  /*********************************************************************/
  /*  Ensure that the module name doesn't have a path.                 */
  /*  This is done to ensure that only approved libs from the system   */
  /*  directories are used (to make this even remotely secure).        */
  /*********************************************************************/
  if (check_valid_path(Module, strlen(Module))) {
    strcpy(g->Message, "Module cannot contain a path");
    return NULL;
  } else
//  PlugSetPath(soname, Module, GetPluginDir());  // Crashes on Fedora
    strncat(strcpy(soname, GetPluginDir()), Module,
			sizeof(soname) - strlen(soname) - 1);

#if defined(_WIN32)
  // Is the DLL already loaded?
  if (!Hdll && !(Hdll = GetModuleHandle(soname)))
    // No, load the Dll implementing the function
    if (!(Hdll = LoadLibrary(soname))) {
      char  buf[256];
      DWORD rc = GetLastError();

      sprintf(g->Message, MSG(DLL_LOAD_ERROR), rc, soname);
      FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                    FORMAT_MESSAGE_IGNORE_INSERTS, NULL, rc, 0,
                    (LPTSTR)buf, sizeof(buf), NULL);
      strcat(strcat(g->Message, ": "), buf);
      return NULL;
      } // endif hDll

  // The exported name is always in uppercase
  for (int i = 0; ; i++) {
    c = Subtype[i];
    getname[i + 3] = toupper(c);
    if (!c) break;
    } // endfor i

  // Get the function returning an instance of the external DEF class
  if (!(getdef = (XGETDEF)GetProcAddress((HINSTANCE)Hdll, getname))) {
    char  buf[256];
    DWORD rc = GetLastError();

    sprintf(g->Message, MSG(PROCADD_ERROR), rc, getname);
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
      FORMAT_MESSAGE_IGNORE_INSERTS, NULL, rc, 0,
      (LPTSTR)buf, sizeof(buf), NULL);
    strcat(strcat(g->Message, ": "), buf);
    FreeLibrary((HMODULE)Hdll);
    return NULL;
    } // endif getdef
#else   // !_WIN32
  const char *error = NULL;

#if 0  // Don't know what all this stuff does
  Dl_info dl_info;

  // The OEM lib must retrieve exported CONNECT variables
  if (dladdr(&connect_hton, &dl_info)) {
    if (dlopen(dl_info.dli_fname, RTLD_NOLOAD | RTLD_NOW | RTLD_GLOBAL) == 0) {
      error = dlerror();
      sprintf(g->Message, "dlopen failed: %s, OEM not supported", SVP(error));
      return NULL;
      } // endif dlopen

  } else {
    error = dlerror();
    sprintf(g->Message, "dladdr failed: %s, OEM not supported", SVP(error));
    return NULL;
  } // endif dladdr
#endif // 0

  // Load the desired shared library
  if (!Hdll && !(Hdll = dlopen(soname, RTLD_LAZY))) {
    error = dlerror();
    sprintf(g->Message, MSG(SHARED_LIB_ERR), soname, SVP(error));
    return NULL;
    } // endif Hdll

  // The exported name is always in uppercase
  for (int i = 0; ; i++) {
    c = Subtype[i];
    getname[i + 3] = toupper(c);
    if (!c) break;
    } // endfor i

  // Get the function returning an instance of the external DEF class
  if (!(getdef = (XGETDEF)dlsym(Hdll, getname))) {
    error = dlerror();
    sprintf(g->Message, MSG(GET_FUNC_ERR), getname, SVP(error));
    dlclose(Hdll);
    return NULL;
    } // endif getdef
#endif  // !_WIN32

  // Just in case the external Get function does not set error messages
  sprintf(g->Message, MSG(DEF_ALLOC_ERROR), Subtype);

  // Get the table definition block
  if (!(xdefp = getdef(g, NULL)))
    return NULL;

  // Have the external class do its complete definition
  if (!cat->Cbuf) {
    // Suballocate a temporary buffer for the entire column section
    cat->Cblen = GetSizeCatInfo("Colsize", "8K");
    cat->Cbuf = (char*)PlugSubAlloc(g, NULL, cat->Cblen);
    } // endif Cbuf

  // Ok, return external block
  return xdefp;
  } // end of GetXdef

#if 0
/***********************************************************************/
/*  DeleteTableFile: Delete an OEM table file if applicable.           */
/***********************************************************************/
bool OEMDEF::DeleteTableFile(PGLOBAL g)
  {
  if (!Pxdef)
    Pxdef = GetXdef(g);

  return (Pxdef) ? Pxdef->DeleteTableFile(g) : true;
  } // end of DeleteTableFile
#endif // 0

/***********************************************************************/
/*  Define: initialize the table definition block from XDB file.       */
/***********************************************************************/
bool OEMDEF::DefineAM(PGLOBAL g, LPCSTR, int)
  {
	Module = GetStringCatInfo(g, "Module", "");
  Subtype = GetStringCatInfo(g, "Subtype", Module);

  if (!*Module)
    Module = Subtype;

  char *desc = (char*)PlugSubAlloc(g, NULL, strlen(Module)
                                          + strlen(Subtype) + 3);
  sprintf(desc, "%s(%s)", Module, Subtype);
  Desc = desc;

	// If define block not here yet, get it now
	if (!Pxdef && !(Pxdef = GetXdef(g)))
		return true;            // Error

	// Here "OEM" should be replace by a more useful value
  return Pxdef->Define(g, Cat, Name, Schema, Subtype);
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB OEMDEF::GetTable(PGLOBAL g, MODE mode)
  {
  PTDB  tdbp = NULL;

  // If define block not here yet, get it now
  if (!Pxdef && !(Pxdef = GetXdef(g)))
    return NULL;            // Error

  /*********************************************************************/
  /*  Allocate a TDB of the proper type.                               */
  /*  Column blocks will be allocated only when needed.                */
  /*********************************************************************/
  if (!(tdbp = Pxdef->GetTable(g, mode)))
    return NULL;
  else if (Multiple && tdbp->GetFtype() == RECFM_OEM)
    tdbp = new(g) TDBMUL(tdbp);       // No block optimization yet

#if 0
  /*********************************************************************/
  /*  The OEM table is based on a file type (currently DOS+ only)      */
  /*********************************************************************/
  assert (rfm == RECFM_VAR || rfm == RECFM_FIX ||
          rfm == RECFM_BIN || rfm == RECFM_VCT);

  PTXF    txfp = NULL;
  PDOSDEF defp = (PDOSDEF)Pxdef;
  bool    map = defp->Mapped && mode != MODE_INSERT &&
                !(UseTemp() == TMP_FORCE &&
                (mode == MODE_UPDATE || mode == MODE_DELETE));
  int     cmpr = defp->Compressed;

  /*********************************************************************/
  /*  Allocate table and file processing class of the proper type.     */
  /*  Column blocks will be allocated only when needed.                */
  /*********************************************************************/
  if (!((PTDBDOS)tdbp)->GetTxfp()) {
    if (cmpr) {
#if defined(GZ_SUPPORT)
      if (cmpr == 1)
        txfp = new(g) GZFAM(defp);
      else
        txfp = new(g) ZLBFAM(defp);
#else   // !GZ_SUPPORT
      strcpy(g->Message, "Compress not supported");
      return NULL;
#endif  // !GZ_SUPPORT
    } else if (rfm == RECFM_VAR) {
      if (map)
        txfp = new(g) MAPFAM(defp);
      else
        txfp = new(g) DOSFAM(defp);

    } else if (rfm == RECFM_FIX || rfm == RECFM_BIN) {
      if (map)
        txfp = new(g) MPXFAM(defp);
      else
        txfp = new(g) FIXFAM(defp);
    } else if (rfm == RECFM_VCT) {
#if defined(VCT_SUPPORT)
      assert(Pxdef->GetDefType() == TYPE_AM_VCT);

      if (map)
        txfp = new(g) VCMFAM((PVCTDEF)defp);
      else
        txfp = new(g) VCTFAM((PVCTDEF)defp);
#else   // !VCT_SUPPORT
      strcpy(g->Message, "VCT no more supported");
      return NULL;
#endif  // !VCT_SUPPORT
    } // endif's

    ((PTDBDOS)tdbp)->SetTxfp(txfp);
    } // endif Txfp

  if (Multiple)
    tdbp = new(g) TDBMUL(tdbp);
#endif // 0
  return tdbp;
  } // end of GetTable

/* --------------------------- Class COLCRT -------------------------- */

/***********************************************************************/
/*  COLCRT Constructors.                                               */
/***********************************************************************/
COLCRT::COLCRT(PSZ name)
  {
  Next = NULL;
  Name = name;
  Desc = NULL;
  Decode = NULL;
  Fmt = NULL;
  Offset = -1;
  Long = -1;
  Precision = -1;
  Freq = -1;
  Key = -1;
  Scale = -1;
  Opt = -1;
  DataType = '*';
  } // end of COLCRT constructor for table creation

COLCRT::COLCRT(void)
  {
  Next = NULL;
  Name = NULL;
  Desc = NULL;
  Decode = NULL;
  Fmt = NULL;
  Offset = 0;
  Long = 0;
  Precision = 0;
  Freq = 0;
  Key = 0;
  Scale = 0;
  Opt = 0;
  DataType = '*';
  } // end of COLCRT constructor for table & view definition

/* --------------------------- Class COLDEF -------------------------- */

/***********************************************************************/
/*  COLDEF Constructor.                                                */
/***********************************************************************/
COLDEF::COLDEF(void) : COLCRT()
  {
  To_Min = NULL;
  To_Max = NULL;
  To_Pos = NULL;
  Xdb2 = FALSE;
  To_Bmap = NULL;
  To_Dval = NULL;
  Ndv = 0;
  Nbm = 0;
  Buf_Type = TYPE_ERROR;
  Clen = 0;
  Poff = 0;
  memset(&F, 0, sizeof(FORMAT));
  Flags = 0;
  } // end of COLDEF constructor

/***********************************************************************/
/*  Define: initialize a column definition from a COLINFO structure.   */
/***********************************************************************/
int COLDEF::Define(PGLOBAL g, void *, PCOLINFO cfp, int poff)
  {
  Name = (PSZ)PlugDup(g, cfp->Name);

  if (!(cfp->Flags & U_SPECIAL)) {
    Poff = poff;
    Buf_Type = cfp->Type;

    if ((Clen = GetTypeSize(Buf_Type, cfp->Length)) < 0) {
      sprintf(g->Message, MSG(BAD_COL_TYPE), GetTypeName(Buf_Type), Name);
      return -1;
      } // endswitch

    strcpy(F.Type, GetFormatType(Buf_Type));
    F.Length = cfp->Length;
    F.Prec = cfp->Scale;
    Offset = (cfp->Offset < 0) ? poff : cfp->Offset;
    Precision = cfp->Precision;
    Scale = cfp->Scale;
    Long = cfp->Length;
    Opt = cfp->Opt;
    Key = cfp->Key;
    Freq = cfp->Freq;

    if (cfp->Remark && *cfp->Remark)
      Desc = (PSZ)PlugDup(g, cfp->Remark);

    if (cfp->Datefmt)
      Decode = (PSZ)PlugDup(g, cfp->Datefmt);

  } else
    Offset = poff;

  if (cfp->Fieldfmt)
    Fmt = (PSZ)PlugDup(g, cfp->Fieldfmt);

  Flags = cfp->Flags;
  return (Flags & (U_VIRTUAL|U_SPECIAL)) ? 0 : Long;
  } // end of Define

/* ------------------------- End of RelDef --------------------------- */
