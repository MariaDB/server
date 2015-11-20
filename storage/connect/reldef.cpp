/************* RelDef CPP Program Source Code File (.CPP) **************/
/* PROGRAM NAME: RELDEF                                                */
/* -------------                                                       */
/*  Version 1.4                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2004-2015    */
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
#if defined(__WIN__)
#include <sqlext.h>
#else
#include <dlfcn.h>          // dlopen(), dlclose(), dlsym() ...
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
#include "filamap.h"
#include "filamfix.h"
#include "filamvct.h"
#if defined(ZIP_SUPPORT)
#include "filamzip.h"
#endif   // ZIP_SUPPORT
#include "tabdos.h"
#include "valblk.h"
#include "tabmul.h"
#include "ha_connect.h"
#include "mycat.h"

#if !defined(__WIN__)
extern handlerton *connect_hton;
#endif   // !__WIN__

/***********************************************************************/
/*  External function.                                                 */
/***********************************************************************/
USETEMP UseTemp(void);
char   *GetPluginDir(void);

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
bool RELDEF::SetIntCatInfo(PSZ what, int n)
	{
	return Hc->SetIntegerOption(what, n);
	} // end of SetIntCatInfo

/***********************************************************************/
/*  This function returns integer table information.                   */
/***********************************************************************/
int RELDEF::GetIntCatInfo(PSZ what, int idef)
	{
	int n= Hc->GetIntegerOption(what);

	return (n == NO_IVAL) ? idef : n;
	} // end of GetIntCatInfo

/***********************************************************************/
/*  This function returns Boolean table information.                   */
/***********************************************************************/
bool RELDEF::GetBoolCatInfo(PSZ what, bool bdef)
	{
	bool b= Hc->GetBooleanOption(what, bdef);

	return b;
	} // end of GetBoolCatInfo

/***********************************************************************/
/*  This function returns size catalog information.                    */
/***********************************************************************/
int RELDEF::GetSizeCatInfo(PSZ what, PSZ sdef)
	{
	char * s, c;
  int  i, n= 0;

	if (!(s= Hc->GetStringOption(what)))
		s= sdef;

	if ((i= sscanf(s, " %d %c ", &n, &c)) == 2)
    switch (toupper(c)) {
      case 'M':
        n *= 1024;
      case 'K':
        n *= 1024;
      } // endswitch c

  return n;
} // end of GetSizeCatInfo

/***********************************************************************/
/*  This function sets char table information in buf.                  */
/***********************************************************************/
int RELDEF::GetCharCatInfo(PSZ what, PSZ sdef, char *buf, int size)
	{
	char *s= Hc->GetStringOption(what);

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
char *RELDEF::GetStringCatInfo(PGLOBAL g, PSZ what, PSZ sdef)
	{
	char *name, *sval= NULL, *s= Hc->GetStringOption(what, sdef);
	
	if (s) {
    if (!Hc->IsPartitioned() ||
        (stricmp(what, "filename") && stricmp(what, "tabname")
                                   && stricmp(what, "connect")))
		  sval= PlugDup(g, s);
    else
      sval= s;

  } else if (!stricmp(what, "filename")) {
    // Return default file name
    char *ftype= Hc->GetStringOption("Type", "*");
    int   i, n;

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
	}	// end of GetStringCatInfo

/* --------------------------- Class TABDEF -------------------------- */

/***********************************************************************/
/*  TABDEF Constructor.                                                */
/***********************************************************************/
TABDEF::TABDEF(void)
  {
  Schema = NULL;
  Desc = NULL;
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
/*  Define: initialize the table definition block from XDB file.       */
/***********************************************************************/
bool TABDEF::Define(PGLOBAL g, PCATLG cat, LPCSTR name, LPCSTR am)
  {
  int   poff = 0;

  Name = (PSZ)PlugDup(g, name);
  Cat = cat;
  Hc = ((MYCAT*)cat)->GetHandler();
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

  // Get The column definitions
  if ((poff = GetColCatInfo(g)) < 0)
    return true;

  // Do the definition of AM specific fields
  return DefineAM(g, am, poff);
  } // end of Define

/***********************************************************************/
/*  This function returns the database data path.                      */
/***********************************************************************/
PSZ TABDEF::GetPath(void)
  {
  return (Database) ? (PSZ)Database : (Hc) ? Hc->GetDataPath() : NULL;
  } // end of GetPath

/***********************************************************************/
/*  This function returns column table information.                    */
/***********************************************************************/
int TABDEF::GetColCatInfo(PGLOBAL g)
	{
	char		*type= GetStringCatInfo(g, "Type", "*");
  char     c, fty, eds;
	int      i, n, loff, poff, nof, nlg;
	void    *field= NULL;
  TABTYPE  tc;
  PCOLDEF  cdp, lcdp= NULL, tocols= NULL;
	PCOLINFO pcf= (PCOLINFO)PlugSubAlloc(g, NULL, sizeof(COLINFO));

  memset(pcf, 0, sizeof(COLINFO));

  // Get a unique char identifier for type
  tc= (Catfunc == FNC_NO) ? GetTypeID(type) : TAB_PRX;

  // Take care of the column definitions
	i= poff= nof= nlg= 0;

	// Offsets of HTML and DIR tables start from 0, DBF at 1
	loff= (tc == TAB_DBF) ? 1 : (tc == TAB_XML || tc == TAB_DIR) ? -1 : 0; 

  while (true) {
		// Default Offset depends on table type
		switch (tc) {
      case TAB_DOS:
      case TAB_FIX:
      case TAB_BIN:
      case TAB_VEC:
      case TAB_DBF:
        poff= loff + nof;				 // Default next offset
				nlg= MY_MAX(nlg, poff);		 // Default lrecl
        break;
      case TAB_CSV:
      case TAB_FMT:
				nlg+= nof;
      case TAB_DIR:
      case TAB_XML:
        poff= loff + 1;
        break;
      case TAB_INI:
      case TAB_MAC:
      case TAB_TBL:
      case TAB_XCL:
      case TAB_OCCUR:
      case TAB_PRX:
      case TAB_OEM:
        poff = 0;      // Offset represents an independant flag
        break;
      default:         // VCT PLG ODBC MYSQL WMI...
        poff = 0;			 // NA
        break;
			} // endswitch tc

//		do {
			field= Hc->GetColumnOption(g, field, pcf);
//    } while (field && (*pcf->Name =='*' /*|| pcf->Flags & U_VIRTUAL*/));

		if (tc == TAB_DBF && pcf->Type == TYPE_DATE && !pcf->Datefmt) {
			// DBF date format defaults to 'YYYMMDD'
			pcf->Datefmt= "YYYYMMDD";
			pcf->Length= 8;
			} // endif tc

		if (!field)
			break;

    // Allocate the column description block
    cdp= new(g) COLDEF;

    if ((nof= cdp->Define(g, NULL, pcf, poff)) < 0)
      return -1;						 // Error, probably unhandled type
		else
			loff= cdp->GetOffset();

		switch (tc) {
			case TAB_VEC:
				cdp->SetOffset(0);		 // Not to have shift
			case TAB_BIN:
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
						case 'I':	nof = sizeof(int);      break;
						case 'D':	nof = sizeof(double);   break;
						case 'S':	nof = sizeof(short);    break;
						case 'T':	nof = sizeof(char);     break;
						case 'G':	nof = sizeof(longlong); break;
						default:  /* Wrong format */
              sprintf(g->Message, "Invalid format %c", fty);
              return -1;
						} // endswitch fty

          } // endif nof

      default:
				break;
			} // endswitch tc

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
		int			ending, recln= 0;

		// Was commented because sometimes ending is 0 even when
		// not specified (for instance if quoted is specified)
//	if ((ending= Hc->GetIntegerOption("Ending")) < 0) {
		if ((ending= Hc->GetIntegerOption("Ending")) <= 0) {
      ending= (tc == TAB_BIN || tc == TAB_VEC) ? 0 : CRLF;
			Hc->SetIntegerOption("Ending", ending);
			} // endif ending

		// Calculate the default record size
		switch (tc) {
      case TAB_FIX:
      case TAB_BIN:
        recln= nlg + ending;     // + length of line ending
        break;
      case TAB_VEC:
        recln= nlg;
	
//      if ((k= (pak < 0) ? 8 : pak) > 1)
          // See above for detailed comment
          // Round up lrecl to multiple of 8 or pak
//        recln= ((recln + k - 1) / k) * k;
	
        break;
      case TAB_DOS:
      case TAB_DBF:
        recln= nlg;
        break;
      case TAB_CSV:
      case TAB_FMT:
        // The number of separators (assuming an extra one can exist)
//      recln= poff * ((qotd) ? 3 : 1);	 to be investigated
				recln= nlg + poff * 3;     // To be safe
      default:
        break;
      } // endswitch tc

		// lrecl must be at least recln to avoid buffer overflow
		recln= MY_MAX(recln, Hc->GetIntegerOption("Lrecl"));
		Hc->SetIntegerOption("Lrecl", recln);
		((PDOSDEF)this)->SetLrecl(recln);
		} // endif Lrecl

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
  /*  Ensure that the .dll doesn't have a path.                        */
  /*  This is done to ensure that only approved dll from the system    */
  /*  directories are used (to make this even remotely secure).        */
  /*********************************************************************/
  if (check_valid_path(Module, strlen(Module))) {
    strcpy(g->Message, "Module cannot contain a path");
    return NULL;
  } else
    PlugSetPath(soname, Module, GetPluginDir());
    
#if defined(__WIN__)
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
    sprintf(g->Message, MSG(PROCADD_ERROR), GetLastError(), getname);
    FreeLibrary((HMODULE)Hdll);
    return NULL;
    } // endif getdef
#else   // !__WIN__
  const char *error = NULL;
  Dl_info dl_info;
    
#if 0  // Don't know what all this stuff does
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

  // Is the library already loaded?
  if (!Hdll && !(Hdll = dlopen(soname, RTLD_NOLOAD)))
    // Load the desired shared library
    if (!(Hdll = dlopen(soname, RTLD_LAZY))) {
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
#endif  // !__WIN__

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

  // Here "OEM" should be replace by a more useful value
  if (xdefp->Define(g, cat, Name, "OEM"))
    return NULL;

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

  Desc = (char*)PlugSubAlloc(g, NULL, strlen(Module)
                                    + strlen(Subtype) + 3);
  sprintf(Desc, "%s(%s)", Module, Subtype);
  return false;
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB OEMDEF::GetTable(PGLOBAL g, MODE mode)
  {
  RECFM   rfm;
  PTDBASE tdbp = NULL;

  // If define block not here yet, get it now
  if (!Pxdef && !(Pxdef = GetXdef(g)))
    return NULL;            // Error

  /*********************************************************************/
  /*  Allocate a TDB of the proper type.                               */
  /*  Column blocks will be allocated only when needed.                */
  /*********************************************************************/
  if (!(tdbp = (PTDBASE)Pxdef->GetTable(g, mode)))
    return NULL;
  else
    rfm = tdbp->GetFtype();

  if (rfm == RECFM_NAF)
    return tdbp;
  else if (rfm == RECFM_OEM) {
    if (Multiple)
      tdbp = new(g) TDBMUL(tdbp);       // No block optimization yet

    return tdbp;
    } // endif OEM

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
#if defined(ZIP_SUPPORT)
      if (cmpr == 1)
        txfp = new(g) ZIPFAM(defp);
      else
        txfp = new(g) ZLBFAM(defp);
#else   // !ZIP_SUPPORT
      strcpy(g->Message, "Compress not supported");
      return NULL;
#endif  // !ZIP_SUPPORT
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
      assert (Pxdef->GetDefType() == TYPE_AM_VCT);

      if (map)
        txfp = new(g) VCMFAM((PVCTDEF)defp);
      else
        txfp = new(g) VCTFAM((PVCTDEF)defp);

    } // endif's

    ((PTDBDOS)tdbp)->SetTxfp(txfp);
    } // endif Txfp

  if (Multiple)
    tdbp = new(g) TDBMUL(tdbp);

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

    if ((Clen = GetTypeSize(Buf_Type, cfp->Length)) <= 0) {
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
