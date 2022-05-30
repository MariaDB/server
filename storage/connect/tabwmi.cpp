/***********************************************************************/
/*  TABWMI: Author Olivier Bertrand -- PlugDB -- 2012 - 2017           */
/*  TABWMI: Virtual table to get WMI information.                      */
/***********************************************************************/
#if !defined(_WIN32)
#error This is a WINDOWS only table type
#endif   // !_WIN32
#include "my_global.h"
#include <stdio.h>

#include "global.h"
#include "plgdbsem.h"
#include "mycat.h"
//#include "reldef.h"
#include "xtable.h"
#include "tabext.h"
#include "colblk.h"
//#include "filter.h"
//#include "xindex.h"
#include "tabwmi.h"
#include "valblk.h"
#include "plgcnx.h"                       // For DB types
#include "resource.h"

/* ------------------- Functions WMI Column info --------------------- */

/***********************************************************************/
/*  Initialize WMI operations.                                         */
/***********************************************************************/
PWMIUT InitWMI(PGLOBAL g, PCSZ nsp, PCSZ classname)
{
  IWbemLocator *loc;
  char         *p;
  HRESULT       res;
  PWMIUT        wp = (PWMIUT)PlugSubAlloc(g, NULL, sizeof(WMIUTIL));

  if (trace(1))
    htrc("WMIColumns class %s space %s\n", SVP(classname), SVP(nsp));

  /*********************************************************************/
  /*  Set default values for the namespace and class name.             */
  /*********************************************************************/
  if (!nsp)
    nsp = "root\\cimv2";

  if (!classname) {
    if (!stricmp(nsp, "root\\cimv2"))
      classname = "ComputerSystemProduct";
    else if (!stricmp(nsp, "root\\cli"))
      classname = "Msft_CliAlias";
    else {
      strcpy(g->Message, "Missing class name");
      return NULL;
      } // endif classname

    } // endif classname

  /*********************************************************************/
  /*  Initialize WMI.                                                  */
  /*********************************************************************/
//res = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  res = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

  if (FAILED(res)) {
    sprintf(g->Message, "Failed to initialize COM library. " 
            "Error code = %x", res);
    return NULL;
    } // endif res

#if 0 // irrelevant for a DLL
  res = CoInitializeSecurity(NULL, -1, NULL, NULL,
                       RPC_C_AUTHN_LEVEL_CONNECT,
                       RPC_C_IMP_LEVEL_IMPERSONATE,
                       NULL, EOAC_NONE, NULL);
  
  if (res != RPC_E_TOO_LATE && FAILED(res)) {
    sprintf(g->Message, "Failed to initialize security. " 
            "Error code = %p", res);
    CoUninitialize();
    return NULL;
    }  // endif Res
#endif // 0

  res = CoCreateInstance(CLSID_WbemLocator, NULL, 
                         CLSCTX_INPROC_SERVER, IID_IWbemLocator, 
                         (void**) &loc);
  if (FAILED(res)) {
    sprintf(g->Message, "Failed to create Locator. " 
            "Error code = %x", res);
    CoUninitialize();
    return NULL;
    }  // endif res
    
  res = loc->ConnectServer(_bstr_t(nsp), 
                    NULL, NULL, NULL, 0, NULL, NULL, &wp->Svc);

  if (FAILED(res)) {
    sprintf(g->Message, "Could not connect. Error code = %x", res); 
    loc->Release();     
    CoUninitialize();
    return NULL;
    }  // endif res

  loc->Release();

  if (trace(1))
    htrc("Successfully connected to namespace.\n");

  /*********************************************************************/
  /*  Perform a full class object retrieval.                           */
  /*********************************************************************/
  p = (char*)PlugSubAlloc(g, NULL, strlen(classname) + 7);

  if (strchr(classname, '_'))
    strcpy(p, classname);
  else
    strcat(strcpy(p, "Win32_"), classname);

  res = wp->Svc->GetObject(bstr_t(p), 0, 0, &wp->Cobj, 0);

  if (FAILED(res)) {
    sprintf(g->Message, "failed GetObject %s in %s\n", classname, nsp);
    wp->Svc->Release();
    wp->Svc = NULL;    // MUST be set to NULL  (why?)
    return NULL;
    }  // endif res

  return wp;
} // end of InitWMI

/***********************************************************************/
/* WMIColumns: constructs the result blocks containing the description */
/* of all the columns of a WMI table of a specified class.             */
/***********************************************************************/
PQRYRES WMIColumns(PGLOBAL g, PCSZ nsp, PCSZ cls, bool info)
  {
  static int  buftyp[] = {TYPE_STRING, TYPE_SHORT, TYPE_STRING,
                          TYPE_INT,   TYPE_INT, TYPE_SHORT};
  static XFLD fldtyp[] = {FLD_NAME, FLD_TYPE,   FLD_TYPENAME,
                          FLD_PREC, FLD_LENGTH, FLD_SCALE};
  static unsigned int len, length[] = {0, 6, 8, 10, 10, 6};
  int     i = 0, n = 0, ncol = sizeof(buftyp) / sizeof(int);
  int     lng, typ, prec;
  LONG    low, upp;
  BSTR    propname;
  VARIANT val;
  CIMTYPE type;
  HRESULT res;
  PWMIUT  wp; 
  SAFEARRAY *prnlist = NULL;
  PQRYRES qrp = NULL;
  PCOLRES crp;

  if (!info) {
    /*******************************************************************/
    /*  Initialize WMI if not done yet.                                */
    /*******************************************************************/
    if (!(wp = InitWMI(g, nsp, cls)))
      return NULL;

    /*******************************************************************/
    /*  Get the number of properties to return.                        */
    /*******************************************************************/
    res = wp->Cobj->Get(bstr_t("__Property_Count"), 0, &val, NULL, NULL);

    if (FAILED(res)) {
      sprintf(g->Message, "failed Get(__Property_Count) res=%d\n", res);
      goto err;
      }  // endif res

    if (!(n = val.lVal)) {
      sprintf(g->Message, "Class %s in %s has no properties\n",
                          cls, nsp);
      goto err;
      }  // endif res

    /*******************************************************************/
    /*  Get max property name length.                                  */
    /*******************************************************************/
    res = wp->Cobj->GetNames(NULL, 
          WBEM_FLAG_ALWAYS | WBEM_FLAG_NONSYSTEM_ONLY, 
          NULL, &prnlist);

    if (FAILED(res)) {
      sprintf(g->Message, "failed GetNames res=%d\n", res);
      goto err;
      }  // endif res

    res = SafeArrayGetLBound(prnlist, 1, &low);
    res = SafeArrayGetUBound(prnlist, 1, &upp);

    for (long i = low; i <= upp; i++) {
      // Get this property name.
      res = SafeArrayGetElement(prnlist, &i, &propname);

      if (FAILED(res)) {
        sprintf(g->Message, "failed GetArrayElement res=%d\n", res);
        goto err;
        }  // endif res

      len = (unsigned)SysStringLen(propname);
      length[0] = MY_MAX(length[0], len);
      } // enfor i

    res = SafeArrayDestroy(prnlist);
  } else
    length[0] = 128;

  /*********************************************************************/
  /*  Allocate the structures used to refer to the result set.         */
  /*********************************************************************/
  qrp = PlgAllocResult(g, ncol, n, IDS_COLUMNS + 3,
                          buftyp, fldtyp, length, false, true);

  if (info || !qrp)
    return qrp;

  /*********************************************************************/
  /*  Now get the results into blocks.                                 */
  /*********************************************************************/
  res = wp->Cobj->BeginEnumeration(WBEM_FLAG_NONSYSTEM_ONLY);

  if (FAILED(res)) {
    sprintf(g->Message, "failed BeginEnumeration hr=%d\n", res);
    qrp = NULL;
    goto err;
    }  // endif hr

  while (TRUE) {
    res = wp->Cobj->Next(0, &propname, &val, &type, NULL);

    if (FAILED(res)) {
      sprintf(g->Message, "failed getting Next hr=%d\n", res);
      qrp = NULL;
      goto err;
    }  else if (res == WBEM_S_NO_MORE_DATA) {
      VariantClear(&val);
      break;
    } // endif res

    if (i >= n)
      break;                 // Should never happen
    else
      prec = 0;

    switch (type) {
      case CIM_STRING:
        typ = TYPE_STRING;
        lng = 255;
        prec = 1;   // Case insensitive
        break;
      case CIM_SINT32:                          
      case CIM_UINT32:
      case CIM_BOOLEAN:
        typ = TYPE_INT;
        lng = 11;
        break;
      case CIM_SINT8:
      case CIM_UINT8:
        typ = TYPE_TINY;
        lng = 4;
        break;
      case CIM_SINT16:
      case CIM_UINT16:
        typ = TYPE_SHORT;
        lng = 6;
        break;
      case CIM_REAL64:
      case CIM_REAL32:
        prec = 2;
        typ = TYPE_DOUBLE;
        lng = 15;
        break;
      case CIM_SINT64:
      case CIM_UINT64:
        typ = TYPE_BIGINT;
        lng = 20;
        break;
      case CIM_DATETIME:
        typ = TYPE_DATE;
        lng = 19;
        break;
      case CIM_CHAR16:
        typ = TYPE_STRING;
        lng = 16;
        break;
      case CIM_EMPTY:
        typ = TYPE_STRING;
        lng = 24;             // ???
        break;
      default:
        qrp->BadLines++;
        goto suite;
      } // endswitch type

    crp = qrp->Colresp;                    // Column Name
    crp->Kdata->SetValue(_com_util::ConvertBSTRToString(propname), i);
    crp = crp->Next;                       // Data Type
    crp->Kdata->SetValue(typ, i);
    crp = crp->Next;                       // Type Name
    crp->Kdata->SetValue(GetTypeName(typ), i);
    crp = crp->Next;                       // Precision
    crp->Kdata->SetValue(lng, i);
    crp = crp->Next;                       // Length
    crp->Kdata->SetValue(lng, i);
    crp = crp->Next;                       // Scale (precision)
    crp->Kdata->SetValue(prec, i);
    i++;

 suite:
    SysFreeString(propname);
    VariantClear(&val);
    } // endfor i

  qrp->Nblin = i;

 err:
  // Cleanup
  wp->Cobj->Release();
  wp->Svc->Release();
  wp->Svc = NULL;    // MUST be set to NULL  (why?)
  CoUninitialize();

  /*********************************************************************/
  /*  Return the result pointer for use by GetData routines.           */
  /*********************************************************************/
  return qrp;
  } // end of WMIColumns

/* -------------- Implementation of the WMI classes  ------------------ */

/***********************************************************************/
/*  DefineAM: define specific AM values for WMI table.                 */
/***********************************************************************/
bool WMIDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
  {
  Nspace = GetStringCatInfo(g, "Namespace", "Root\\CimV2");
  Wclass = GetStringCatInfo(g, "Class",
          (!stricmp(Nspace, "root\\cimv2") ? "ComputerSystemProduct" : 
           !stricmp(Nspace, "root\\cli")   ? "Msft_CliAlias" : ""));

  if (!*Wclass) {
    sprintf(g->Message, "Missing class name for %s", Nspace);
    return true;
  } else if (!strchr(Wclass, '_')) {
    char *p = (char*)PlugSubAlloc(g, NULL, strlen(Wclass) + 7);
    Wclass = strcat(strcpy(p, "Win32_"), Wclass);
  } // endif Wclass

  if (Catfunc == FNC_NO)
    Ems = GetIntCatInfo("Estimate", 100);

  return false;
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new TDB of the proper type.                      */
/***********************************************************************/
PTDB WMIDEF::GetTable(PGLOBAL g, MODE m)
  {
  if (Catfunc == FNC_NO)
    return new(g) TDBWMI(this);
  else if (Catfunc == FNC_COL)
    return new(g) TDBWCL(this);

  sprintf(g->Message, "Bad catfunc %ud for WMI", Catfunc);
  return NULL;
  } // end of GetTable

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Implementation of the TDBWMI class.                                */
/***********************************************************************/
TDBWMI::TDBWMI(PWMIDEF tdp) : TDBASE(tdp)
  {
  Svc = NULL;
  Enumerator = NULL;
  ClsObj = NULL;
  Nspace = tdp->Nspace;
  Wclass = tdp->Wclass;
  ObjPath = NULL;
  Kvp = NULL;
  Ems = tdp->Ems;
  Kcol = NULL;
  Vbp = NULL;
  Init = false;
  Done = false;
  Res = 0;
  Rc = 0;
  N = -1;
  } // end of TDBWMI constructor

/***********************************************************************/
/*  Allocate WMI column description block.                             */
/***********************************************************************/
PCOL TDBWMI::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  PCOL colp;

  colp = new(g) WMICOL(cdp, this, n);

  if (cprec) {
    colp->SetNext(cprec->GetNext());
    cprec->SetNext(colp);
  } else {
    colp->SetNext(Columns);
    Columns = colp;
  } // endif cprec

  return colp;
  } // end of MakeCol

/***********************************************************************/
/*  Initialize: Initialize WMI operations.                             */
/***********************************************************************/
bool TDBWMI::Initialize(PGLOBAL g)
  {
  if (Init)
    return false;

  // Initialize COM.
  Res = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

  if (FAILED(Res)) {
    sprintf(g->Message, "Failed to initialize COM library. " 
            "Error code = %x", Res);
    return true;              // Program has failed.
    } // endif Res

  // Obtain the initial locator to Windows Management
  // on a particular host computer.
  IWbemLocator *loc;        // Initial Windows Management locator

  Res = CoCreateInstance(CLSID_WbemLocator, 0,  CLSCTX_INPROC_SERVER, 
                         IID_IWbemLocator, (LPVOID*) &loc);
 
  if (FAILED(Res)) {
    sprintf(g->Message, "Failed to create Locator. " 
                        "Error code = %x", Res);
    CoUninitialize();
    return true;       // Program has failed.
    }  // endif Res

  // Connect to the specified namespace with the
  // current user and obtain pointer to Svc
  // to make IWbemServices calls.
  Res = loc->ConnectServer(_bstr_t(Nspace),
                             NULL, NULL,0, NULL, 0, 0, &Svc);
  
  if (FAILED(Res)) {
    sprintf(g->Message, "Could not connect. Error code = %x", Res); 
    loc->Release();     
    CoUninitialize();
    return true;                // Program has failed.
    }  // endif hres

  loc->Release();                // Not used anymore

  // Set the IWbemServices proxy so that impersonation
  // of the user (client) occurs.
  Res = CoSetProxyBlanket(Svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, 
                          NULL, RPC_C_AUTHN_LEVEL_CALL,
                          RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);

  if (FAILED(Res)) {
    sprintf(g->Message, "Could not set proxy. Error code = %x", Res);
    Svc->Release();
    CoUninitialize();
    return true;               // Program has failed.
    }  // endif Res

  Init = true;
  return false;
  } // end of Initialize

/***********************************************************************/
/*  Changes '\' into '\\' in the filter.                               */
/***********************************************************************/
void TDBWMI::DoubleSlash(PGLOBAL g)
  {
  if (To_CondFil && strchr(To_CondFil->Body, '\\')) {
    char *body = To_CondFil->Body;
    char *buf = (char*)PlugSubAlloc(g, NULL, strlen(body) * 2);
    int   i = 0, k = 0;

    do {
      if (body[i] == '\\')
        buf[k++] = '\\';

      buf[k++] = body[i];
      } while (body[i++]);

    To_CondFil->Body = buf;
    } // endif To_CondFil

  } // end of DoubleSlash

/***********************************************************************/
/*  MakeWQL: make the WQL statement use with WMI ExecQuery.            */
/***********************************************************************/
char *TDBWMI::MakeWQL(PGLOBAL g)
  {
  char  *colist, *wql/*, *pw = NULL*/;
  int    len, ncol = 0;
  bool   first = true, noloc = false;
  PCOL   colp;

  // Normal WQL statement to retrieve results
  for (colp = Columns; colp; colp = colp->GetNext())
    if (!colp->IsSpecial() && (colp->GetColUse(U_P | U_J_EXT) || noloc))
      ncol++;

  if (ncol) {
    colist = (char*)PlugSubAlloc(g, NULL, (NAM_LEN + 4) * ncol);

    for (colp = Columns; colp; colp = colp->GetNext())
      if (!colp->IsSpecial()) {
        if (colp->GetResultType() == TYPE_DATE)
          ((DTVAL*)colp->GetValue())->SetFormat(g, "YYYYMMDDhhmmss", 19);

        if (colp->GetColUse(U_P | U_J_EXT) || noloc) {
          if (first) {
            strcpy(colist, colp->GetName());
            first = false;
          } else
            strcat(strcat(colist, ", "), colp->GetName());
      
          } // endif ColUse

        } // endif Special

  } else {
    // ncol == 0 can occur for queries such that sql count(*) from...
    // for which we will count the rows from sql * from...
    colist = (char*)PlugSubAlloc(g, NULL, 2);
    strcpy(colist, "*");
  } // endif ncol

  // Below 14 is length of 'select ' + length of ' from ' + 1
  len = (strlen(colist) + strlen(Wclass) + 14);
  len += (To_CondFil ? strlen(To_CondFil->Body) + 7 : 0);
  wql = (char*)PlugSubAlloc(g, NULL, len);
  strcat(strcat(strcpy(wql, "SELECT "), colist), " FROM ");
  strcat(wql, Wclass);

  if (To_CondFil)
    strcat(strcat(wql, " WHERE "), To_CondFil->Body);

  return wql;
  } // end of MakeWQL

/***********************************************************************/
/*  GetWMIInfo: Get info for the WMI class.                            */
/***********************************************************************/
bool TDBWMI::GetWMIInfo(PGLOBAL g)
  {
  if (Done)
    return false;

  char *cmd = MakeWQL(g);

  if (cmd == NULL) {
    sprintf(g->Message, "Error making WQL statement"); 
    Svc->Release();
    CoUninitialize();
    return true;               // Program has failed.
    }  // endif cmd

  // Query for Wclass in Nspace
  Rc = Svc->ExecQuery(bstr_t("WQL"), bstr_t(cmd),
//    WBEM_FLAG_BIDIRECTIONAL | WBEM_FLAG_RETURN_IMMEDIATELY, 
      WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, 
      NULL, &Enumerator);
  
  if (FAILED(Rc)) {
    sprintf(g->Message, "Query %s failed. Error code = %x", cmd, Rc); 
    Svc->Release();
    CoUninitialize();
    return true;               // Program has failed.
    }  // endif Rc

  Done = true;
  return false;
  } // end of GetWMIInfo

/***********************************************************************/
/*  WMI: Get the number returned instances.                            */
/***********************************************************************/
int TDBWMI::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0) {
    /*******************************************************************/
    /*  Loop enumerating to get the count. This is prone to last a     */
    /*  very long time for some classes such as DataFile, this is why  */
    /*  we just return an estimated value that will be ajusted later.  */
    /*******************************************************************/
    MaxSize = Ems;
#if 0
    if (Initialize(g))
      return -1;
    else if (GetWMIInfo(g))
      return -1;
    else
      MaxSize = 0;

    PDBUSER dup = PlgGetUser(g);

    while (Enumerator) {
      Res = Enumerator->Next(WBEM_INFINITE, 1, &ClsObj, &Rc);

      if (Rc == 0)
        break;

      MaxSize++;
      } // endwile Enumerator

    Res = Enumerator->Reset();
#endif // 0
    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  When making a Kindex, must provide the Key column info.            */
/***********************************************************************/
int TDBWMI::GetRecpos(void)
  {
  if (!Kcol || !Vbp) 
    return N;

  Kcol->Reset();
  Kcol->Eval(NULL);
  Vbp->SetValue(Kcol->GetValue(), N);
  return N;
  }  // end of GetRecpos

/***********************************************************************/
/*  WMI Access Method opening routine.                                 */
/***********************************************************************/
bool TDBWMI::OpenDB(PGLOBAL g)
  {
  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open.                                            */
    /*******************************************************************/
    Res = Enumerator->Reset();
    N = 0;
    return false;
    } // endif use

  if (Mode != MODE_READ) {
    /*******************************************************************/
    /* WMI tables cannot be modified.                                  */
    /*******************************************************************/
    strcpy(g->Message, "WMI tables are read only");
    return true;
    } // endif Mode

  if (!To_CondFil && !stricmp(Wclass, "CIM_Datafile")
                  && !stricmp(Nspace, "root\\cimv2")) {
    strcpy(g->Message, 
      "Would last forever when not filtered, use DIR table instead");
    return true;
  } else
    DoubleSlash(g);

  Use = USE_OPEN;       // Do it now in case we are recursively called

  /*********************************************************************/
  /*  Initialize the WMI processing.                                   */
  /*********************************************************************/
  if (Initialize(g))
    return true;
  else
    return GetWMIInfo(g);

  } // end of OpenDB

/***********************************************************************/
/*  Data Base read routine for WMI access method.                      */
/***********************************************************************/
int TDBWMI::ReadDB(PGLOBAL g)
  {
  Res = Enumerator->Next(WBEM_INFINITE, 1, &ClsObj, &Rc);

  if (Rc == 0)
    return RC_EF;

  N++;
  return RC_OK;
  } // end of ReadDB

/***********************************************************************/
/*  WriteDB: Data Base write routine for WMI access methods.           */
/***********************************************************************/
int TDBWMI::WriteDB(PGLOBAL g)
  {
  strcpy(g->Message, "WMI tables are read only");
  return RC_FX;
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for WMI access methods.              */
/***********************************************************************/
int TDBWMI::DeleteDB(PGLOBAL g, int irc)
  {
  strcpy(g->Message, "Delete not enabled for WMI tables");
  return RC_FX;
  } // end of DeleteDB

/***********************************************************************/
/*  Data Base close routine for WMI access method.                     */
/***********************************************************************/
void TDBWMI::CloseDB(PGLOBAL g)
  {
  // Cleanup
  if (ClsObj)
    ClsObj->Release();

  if (Enumerator)
    Enumerator->Release();

  if (Svc)
    Svc->Release();

  CoUninitialize();
  } // end of CloseDB

// ------------------------ WMICOL functions ----------------------------

/***********************************************************************/
/*  WMICOL public constructor.                                         */
/***********************************************************************/
WMICOL::WMICOL(PCOLDEF cdp, PTDB tdbp, int n)
      : COLBLK(cdp, tdbp, n)
  {
  Tdbp = (PTDBWMI)tdbp;
  VariantInit(&Prop);
  Ctype = CIM_ILLEGAL;
  Res = 0;
  } // end of WMICOL constructor

#if 0
/***********************************************************************/
/*  WMICOL constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
WMICOL::WMICOL(WMICOL *col1, PTDB tdbp) : COLBLK(col1, tdbp)
  {
  } // end of WMICOL copy constructor
#endif // 0

/***********************************************************************/
/*  Read the next WMI address elements.                                */
/***********************************************************************/
void WMICOL::ReadColumn(PGLOBAL g)
  {
  // Get the value of the Name property
  Res = Tdbp->ClsObj->Get(_bstr_t(Name), 0, &Prop, &Ctype, 0);

  switch (Prop.vt) {
    case VT_EMPTY:
    case VT_NULL:
    case VT_VOID:
      Value->Reset();
      break;
    case VT_BSTR:
      Value->SetValue_psz(_com_util::ConvertBSTRToString(Prop.bstrVal));
      break;
    case VT_I4:
    case VT_UI4:
      Value->SetValue(Prop.lVal);
      break;
    case VT_I2:
    case VT_UI2:
      Value->SetValue(Prop.iVal);
      break;
    case VT_INT:
    case VT_UINT:
      Value->SetValue((int)Prop.intVal);
      break;
    case VT_BOOL:
      Value->SetValue(((int)Prop.boolVal) ? 1 : 0);
      break;
    case VT_R8:
      Value->SetValue(Prop.dblVal);
      break;
    case VT_R4:
      Value->SetValue((double)Prop.fltVal);
      break;
    case VT_DATE:
      switch (Value->GetType()) {
        case TYPE_DATE:
         {SYSTEMTIME stm;
          struct tm  ptm;
          int        rc = VariantTimeToSystemTime(Prop.date, &stm);

          ptm.tm_year = stm.wYear;
          ptm.tm_mon  = stm.wMonth;
          ptm.tm_mday  = stm.wDay;
          ptm.tm_hour = stm.wHour;
          ptm.tm_min  = stm.wMinute;
          ptm.tm_sec  = stm.wSecond;
          ((DTVAL*)Value)->MakeTime(&ptm);
         }break;
        case TYPE_STRING:
         {SYSTEMTIME stm;
          char       buf[24];
          int        rc = VariantTimeToSystemTime(Prop.date, &stm);

          sprintf(buf, "%02d/%02d/%d %02d:%02d:%02d", 
                       stm.wDay, stm.wMonth, stm.wYear,
                       stm.wHour, stm.wMinute, stm.wSecond);
          Value->SetValue_psz(buf);
         }break;
        default:
          Value->SetValue((double)Prop.fltVal);
        } // endswitch Type

      break;
    default:
      // This will reset numeric column value
      Value->SetValue_psz("Type not supported");
      break;
    } // endswitch vt

  VariantClear(&Prop);
  } // end of ReadColumn

/* ---------------------------TDBWCL class --------------------------- */

/***********************************************************************/
/*  TDBWCL class constructor.                                          */
/***********************************************************************/
TDBWCL::TDBWCL(PWMIDEF tdp) : TDBCAT(tdp)
  {
  Nsp = tdp->Nspace; 
  Cls = tdp->Wclass;
  } // end of TDBWCL constructor

/***********************************************************************/
/*  GetResult: Get the list of the WMI class properties.               */
/***********************************************************************/
PQRYRES TDBWCL::GetResult(PGLOBAL g)
  {
  return WMIColumns(g, Nsp, Cls, false);
	} // end of GetResult


