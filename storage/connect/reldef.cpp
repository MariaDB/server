/************* RelDef CPP Program Source Code File (.CPP) **************/
/* PROGRAM NAME: REFDEF                                                */
/* -------------                                                       */
/*  Version 1.3                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2004-2012    */
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
#if defined(WIN32)
#include <sqlext.h>
#else
#include <dlfcn.h>          // dlopen(), dlclose(), dlsym() ...
#include "osutil.h"
//#include "sqlext.h"
#endif

/***********************************************************************/
/*  Include application header files                                   */
/*                                                                     */
/*  global.h     is header containing all global declarations.         */
/*  plgdbsem.h   is header containing DB application declarations.     */
/*  catalog.h    is header containing DB description declarations.     */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "mycat.h"
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
  } // end of RELDEF constructor

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
  } // end of TABDEF constructor

/***********************************************************************/
/*  Define: initialize the table definition block from XDB file.       */
/***********************************************************************/
bool TABDEF::Define(PGLOBAL g, PCATLG cat, LPCSTR name, LPCSTR am)
  {
  int   poff = 0;

  Name = (PSZ)PlugSubAlloc(g, NULL, strlen(name) + 1);
  strcpy(Name, name);
  Cat = cat;
  Catfunc = GetFuncID(Cat->GetStringCatInfo(g, "Catfunc", NULL));
  Elemt = cat->GetIntCatInfo("Elements", 0);
  Multiple = cat->GetIntCatInfo("Multiple", 0);
  Degree = cat->GetIntCatInfo("Degree", 0);
  Read_Only = cat->GetBoolCatInfo("ReadOnly", false);
  const char *data_charset_name= cat->GetStringCatInfo(g, "Data_charset", NULL);
  m_data_charset= data_charset_name ?
                  get_charset_by_csname(data_charset_name, MY_CS_PRIMARY, 0):
                  NULL;

  // Get The column definitions
  if ((poff = cat->GetColCatInfo(g, this)) < 0)
    return true;

  // Do the definition of AM specific fields
  return DefineAM(g, am, poff);
  } // end of Define

/* --------------------------- Class OEMDEF -------------------------- */

/***********************************************************************/
/*  GetXdef: get the external TABDEF from OEM module.                  */
/***********************************************************************/
PTABDEF OEMDEF::GetXdef(PGLOBAL g)
  {
  typedef PTABDEF (__stdcall *XGETDEF) (PGLOBAL, void *);
  char    c, getname[40] = "Get";
  PTABDEF xdefp;
  XGETDEF getdef = NULL;
  PCATLG  cat = Cat;

#if defined(WIN32)
  // Is the DLL already loaded?
  if (!Hdll && !(Hdll = GetModuleHandle(Module)))
    // No, load the Dll implementing the function
    if (!(Hdll = LoadLibrary(Module))) {
      char  buf[256];
      DWORD rc = GetLastError();

      sprintf(g->Message, MSG(DLL_LOAD_ERROR), rc, Module);
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
#else   // !WIN32
  const char *error = NULL;
  // Is the library already loaded?
//  if (!Hdll && !(Hdll = ???))
  // Load the desired shared library
  if (!(Hdll = dlopen(Module, RTLD_LAZY))) {
    error = dlerror();
    sprintf(g->Message, MSG(SHARED_LIB_ERR), Module, SVP(error));
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
#endif  // !WIN32

  // Just in case the external Get function does not set error messages
  sprintf(g->Message, MSG(DEF_ALLOC_ERROR), Subtype);

  // Get the table definition block
  if (!(xdefp = getdef(g, NULL)))
    return NULL;

  // Have the external class do its complete definition
  if (!cat->Cbuf) {
    // Suballocate a temporary buffer for the entire column section
    cat->Cblen = cat->GetSizeCatInfo("Colsize", "8K");
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
bool OEMDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
  {
  Module = Cat->GetStringCatInfo(g, "Module", "");
  Subtype = Cat->GetStringCatInfo(g, "Subtype", Module);

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
                !(PlgGetUser(g)->UseTemp == TMP_FORCE &&
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
      else {
        strcpy(g->Message, "Compress 2 not supported yet");
        return NULL;
      } // endelse
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
  Key = -1;
  Scale = -1;
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
  Key = 0;
  Scale = 0;
  DataType = '*';
  } // end of COLCRT constructor for table & view definition

/* --------------------------- Class COLDEF -------------------------- */

/***********************************************************************/
/*  COLDEF Constructor.                                                */
/***********************************************************************/
COLDEF::COLDEF(void) : COLCRT()
  {
  Buf_Type = TYPE_ERROR;
  Clen = 0;
  Poff = 0;
  memset(&F, 0, sizeof(FORMAT));
  Flags = 0;
  } // end of COLDEF constructor

/***********************************************************************/
/*  Define: initialize a column definition from a COLINFO structure.   */
/***********************************************************************/
int COLDEF::Define(PGLOBAL g, void *memp, PCOLINFO cfp, int poff)
  {
  Name = (PSZ)PlugSubAlloc(g, memp, strlen(cfp->Name) + 1);
  strcpy(Name, cfp->Name);

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
    Key = cfp->Key;

    if (cfp->Remark && *cfp->Remark) {
      Desc = (PSZ)PlugSubAlloc(g, memp, strlen(cfp->Remark) + 1);
      strcpy(Desc, cfp->Remark);
      } // endif Remark

    if (cfp->Datefmt) {
      Decode = (PSZ)PlugSubAlloc(g, memp, strlen(cfp->Datefmt) + 1);
      strcpy(Decode, cfp->Datefmt);
      } // endif Datefmt

    } // endif special

  if (cfp->Fieldfmt) {
    Fmt = (PSZ)PlugSubAlloc(g, memp, strlen(cfp->Fieldfmt) + 1);
    strcpy(Fmt, cfp->Fieldfmt);
    } // endif Fieldfmt

  Flags = cfp->Flags;
  return (Flags & (U_VIRTUAL|U_SPECIAL)) ? 0 : Long;
  } // end of Define

/* ------------------------- End of RelDef --------------------------- */
