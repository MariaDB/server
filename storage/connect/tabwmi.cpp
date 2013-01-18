/***********************************************************************/
/*  TABWMI: Author Olivier Bertrand -- PlugDB -- 2012                  */
/*  TABWMI: Virtual table to get WMI information.                      */
/***********************************************************************/
#if !defined(WIN32)
#error This is a WIN32 only table type
#endif   // !WIN32
#include "my_global.h"
#include <stdio.h>

#include "global.h"
#include "plgdbsem.h"
//#include "catalog.h"
#include "reldef.h"
#include "xtable.h"
#include "colblk.h"
#include "filter.h"
//#include "xindex.h"
#include "tabwmi.h"
#include "valblk.h"
#include "plgcnx.h"                       // For DB types
#include "resource.h"

/* -------------- Implementation of the WMI classes	------------------ */

/***********************************************************************/
/*  DefineAM: define specific AM values for WMI table.                 */
/***********************************************************************/
bool WMIDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
  {
  Nspace = Cat->GetStringCatInfo(g, Name, "Namespace", "Root\\CimV2");
  Wclass = Cat->GetStringCatInfo(g, Name, "Class",
		      (!stricmp(Nspace, "root\\cimv2") ? "ComputerSystemProduct" : 
					 !stricmp(Nspace, "root\\cli")   ? "Msft_CliAlias" : ""));

	if (!*Wclass) {
		sprintf(g->Message, "Missing class name for %s", Nspace);
		return true;
	} else if (!strchr(Wclass, '_')) {
		char *p = (char*)PlugSubAlloc(g, NULL, strlen(Wclass) + 7);
		Wclass = strcat(strcpy(p, "Win32_"), Wclass);
	} // endif Wclass

	if (!(Info = Cat->GetBoolCatInfo(Name, "Info", false)))
		Ems = Cat->GetIntCatInfo(Name, "Estimate", 100);

  return false;
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new TDB of the proper type.                      */
/***********************************************************************/
PTDB WMIDEF::GetTable(PGLOBAL g, MODE m)
  {
	if (Info)
		return new(g) TDBWCL(this);
	else
		return new(g) TDBWMI(this);

  } // end of GetTable

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Implementation of the TDBWMI class.                               */
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
						"Error code = %p", Res);
    return true;              // Program has failed.
	  } // endif Res

  // Obtain the initial locator to Windows Management
  // on a particular host computer.
  IWbemLocator *loc;		  	// Initial Windows Management locator

  Res = CoCreateInstance(CLSID_WbemLocator, 0,  CLSCTX_INPROC_SERVER, 
                         IID_IWbemLocator, (LPVOID*) &loc);
 
  if (FAILED(Res)) {
    sprintf(g->Message, "Failed to create Locator. " 
												"Error code = %p", Res);
    CoUninitialize();
    return true;       // Program has failed.
		}	// endif Res

  // Connect to the specified namespace with the
  // current user and obtain pointer to Svc
  // to make IWbemServices calls.
  Res = loc->ConnectServer(_bstr_t(Nspace),
														 NULL, NULL,0, NULL, 0, 0, &Svc);
  
  if (FAILED(Res)) {
    sprintf(g->Message, "Could not connect. Error code = %p", Res); 
    loc->Release();     
    CoUninitialize();
    return true;                // Program has failed.
		}	// endif hres

  loc->Release();								// Not used anymore

  // Set the IWbemServices proxy so that impersonation
  // of the user (client) occurs.
  Res = CoSetProxyBlanket(Svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, 
													NULL, RPC_C_AUTHN_LEVEL_CALL,
											    RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);

  if (FAILED(Res)) {
    sprintf(g->Message, "Could not set proxy. Error code = 0x", Res);
    Svc->Release();
    CoUninitialize();
    return true;               // Program has failed.
		}	// endif Res

	Init = true;
	return false;
	} // end of Initialize

/***********************************************************************/
/*  Changes '\' into '\\' in the filter.                               */
/***********************************************************************/
void TDBWMI::DoubleSlash(PGLOBAL g)
	{
	if (To_Filter && strchr(To_Filter, '\\')) {
		char *buf = (char*)PlugSubAlloc(g, NULL, strlen(To_Filter) * 2);
		int   i = 0, k = 0;

		do {
			if (To_Filter[i] == '\\')
				buf[k++] = '\\';

			buf[k++] = To_Filter[i];
			} while (To_Filter[i++]);

		To_Filter = buf;
		} // endif To_Filter

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
	len += (To_Filter ? strlen(To_Filter) + 7 : 0);
  wql = (char*)PlugSubAlloc(g, NULL, len);
  strcat(strcat(strcpy(wql, "SELECT "), colist), " FROM ");
  strcat(wql, Wclass);

	if (To_Filter)
	  strcat(strcat(wql, " WHERE "), To_Filter);

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
		}	// endif cmd

  // Query for Wclass in Nspace
  Rc = Svc->ExecQuery(bstr_t("WQL"), bstr_t(cmd),
//    WBEM_FLAG_BIDIRECTIONAL | WBEM_FLAG_RETURN_IMMEDIATELY, 
      WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, 
      NULL, &Enumerator);
  
  if (FAILED(Rc)) {
    sprintf(g->Message, "Query %s failed. Error code = %p", cmd, Rc); 
    Svc->Release();
    CoUninitialize();
    return true;               // Program has failed.
		}	// endif Rc

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
	}	// end of GetRecpos

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

	if (!To_Filter && !stricmp(Wclass, "CIM_Datafile")
								 && !stricmp(Nspace, "root\\cimv2")) {
		strcpy(g->Message, 
			"Would last forever when not filtered, use DIR table instead");
		return true;
	} else
		DoubleSlash(g);

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
					ptm.tm_mday	= stm.wDay;
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
/*  Implementation of the TDBWCL class.                                */
/***********************************************************************/
TDBWCL::TDBWCL(PWMIDEF tdp) : TDBASE(tdp)
  {
  Svc = NULL;
  ClsObj = NULL;
	Propname = NULL;
	Nspace = tdp->Nspace;
	Wclass = tdp->Wclass;
	Init = false;
	Done = false;
  Res = 0;
	N = -1;
	Lng = 0;
	Typ = 0;
	Prec = 0;
  } // end of TDBWCL constructor

/***********************************************************************/
/*  Allocate WCL column description block.                             */
/***********************************************************************/
PCOL TDBWCL::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
	{
	PWCLCOL colp;

	colp = (PWCLCOL)new(g) WCLCOL(cdp, this, n);

	if (cprec) {
		colp->SetNext(cprec->GetNext());
		cprec->SetNext(colp);
	} else {
		colp->SetNext(Columns);
		Columns = colp;
	} // endif cprec

	if (!colp->Flag) {
		if (!stricmp(colp->Name, "Column_Name"))
			colp->Flag = 1;
		else if (!stricmp(colp->Name, "Data_Type"))
			colp->Flag = 2;
		else if (!stricmp(colp->Name, "Type_Name"))
			colp->Flag = 3;
		else if (!stricmp(colp->Name, "Precision"))
			colp->Flag = 4;
		else if (!stricmp(colp->Name, "Length"))
			colp->Flag = 5;
		else if (!stricmp(colp->Name, "Scale"))
			colp->Flag = 6;

		} // endif Flag

	return colp;
	} // end of MakeCol

/***********************************************************************/
/*  Initialize: Initialize WMI operations.                             */
/***********************************************************************/
bool TDBWCL::Initialize(PGLOBAL g)
  {
	if (Init)
		return false;

  // Initialize COM.
  Res = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

  if (FAILED(Res)) {
    sprintf(g->Message, "Failed to initialize COM library. " 
						"Error code = %p", Res);
    return true;              // Program has failed.
	  } // endif Res

  // Obtain the initial locator to Windows Management
  // on a particular host computer.
  IWbemLocator *loc;		  	// Initial Windows Management locator

  Res = CoCreateInstance(CLSID_WbemLocator, 0,  CLSCTX_INPROC_SERVER, 
                         IID_IWbemLocator, (LPVOID*) &loc);
 
  if (FAILED(Res)) {
    sprintf(g->Message, "Failed to create Locator. " 
												"Error code = %p", Res);
    CoUninitialize();
    return true;       // Program has failed.
		}	// endif Res

  // Connect to the specified namespace with the
  // current user and obtain pointer to Svc
  // to make IWbemServices calls.
  Res = loc->ConnectServer(_bstr_t(Nspace),
														 NULL, NULL,0, NULL, 0, 0, &Svc);
  
  if (FAILED(Res)) {
    sprintf(g->Message, "Could not connect. Error code = %p", Res); 
    loc->Release();     
    CoUninitialize();
    return true;                // Program has failed.
		}	// endif hres

  loc->Release();								// Not used anymore

  // Perform a full class object retrieval
  Res = Svc->GetObject(bstr_t(Wclass), 0, 0, &ClsObj, 0);

  if (FAILED(Res)) {
    sprintf(g->Message, "failed GetObject %s in %s\n", Wclass, Nspace);
	  Svc->Release();
		Svc = NULL;    // MUST be set to NULL	(why?)
		return true;
		}	// endif res

	Init = true;
	return false;
	} // end of Initialize

/***********************************************************************/
/*  WCL: Get the number of properties.                                 */
/***********************************************************************/
int TDBWCL::GetMaxSize(PGLOBAL g)
  {
	if (MaxSize < 0) {
		VARIANT val;

		if (Initialize(g))
			return -1;

	  Res = ClsObj->Get(bstr_t("__Property_Count"), 0, &val, NULL, NULL);

	  if (FAILED(Res)) {
	    sprintf(g->Message, "failed Get(Property_Count) res=%d\n", Res);
			return -1;
			}	// endif Res

		MaxSize = val.lVal;
		} // endif MaxSize

	return MaxSize;
	} // end of GetMaxSize

/***********************************************************************/
/*  WCL Access Method opening routine.                                 */
/***********************************************************************/
bool TDBWCL::OpenDB(PGLOBAL g)
  {
  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open.                                            */
    /*******************************************************************/
		ClsObj->BeginEnumeration(WBEM_FLAG_NONSYSTEM_ONLY);
		N = 0;
    return false;
    } // endif use

  if (Mode != MODE_READ) {
    /*******************************************************************/
    /* WMI tables cannot be modified.                                  */
    /*******************************************************************/
    strcpy(g->Message, "WCL tables are read only");
    return true;
    } // endif Mode

  /*********************************************************************/
  /*  Initialize the WMI processing.                                   */
  /*********************************************************************/
	if (Initialize(g))
		return true;

	Res = ClsObj->BeginEnumeration(WBEM_FLAG_NONSYSTEM_ONLY);

  if (FAILED(Res)) {
    sprintf(g->Message, "failed BeginEnumeration hr=%d\n", Res);
		return NULL;
		}	// endif hr

	return false;
  } // end of OpenDB

/***********************************************************************/
/*  Data Base read routine for WCL access method.                      */
/***********************************************************************/
int TDBWCL::ReadDB(PGLOBAL g)
  {
	VARIANT val;
	CIMTYPE type;

	Res = ClsObj->Next(0, &Propname, &val, &type, NULL);

	if (FAILED(Res)) {
	  sprintf(g->Message, "failed getting Next hr=%d\n", Res);
		return RC_FX;
	}	else if (Res == WBEM_S_NO_MORE_DATA) {
    VariantClear(&val);
    return RC_EF;
	} // endif res

	Prec = 0;

	switch (type) {
		case CIM_STRING:
			Typ = TYPE_STRING;
			Lng = 255;
			Prec = 1;   // Case insensitive
			break;
		case CIM_SINT32:													
		case CIM_UINT32:
		case CIM_BOOLEAN:
			Typ = TYPE_INT;
			Lng = 9;
			break;
		case CIM_SINT8:
		case CIM_UINT8:
		case CIM_SINT16:
		case CIM_UINT16:
			Typ = TYPE_SHORT;
			Lng = 6;
			break;
		case CIM_REAL64:
		case CIM_REAL32:
			Prec = 2;
		case CIM_SINT64:
		case CIM_UINT64:
			Typ = TYPE_FLOAT;
			Lng = 15;
			break;
		case CIM_DATETIME:
			Typ = TYPE_DATE;
			Lng = 19;
			break;
		case CIM_CHAR16:
			Typ = TYPE_STRING;
			Lng = 16;
			break;
		case CIM_EMPTY:
			Typ = TYPE_STRING;
			Lng = 24;						 // ???
			break;
		default:
			return RC_NF;
		} // endswitch type

	N++;
	return RC_OK;
  } // end of ReadDB

/***********************************************************************/
/*  WriteDB: Data Base write routine for WCL access methods.           */
/***********************************************************************/
int TDBWCL::WriteDB(PGLOBAL g)
  {
	strcpy(g->Message, "WCL tables are read only");
  return RC_FX;
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for WCL access methods.              */
/***********************************************************************/
int TDBWCL::DeleteDB(PGLOBAL g, int irc)
  {
  strcpy(g->Message, "Delete not enabled for WCL tables");
  return RC_FX;
  } // end of DeleteDB

/***********************************************************************/
/*  Data Base close routine for WMI access method.                     */
/***********************************************************************/
void TDBWCL::CloseDB(PGLOBAL g)
  {
  // Cleanup
	if (ClsObj)
		ClsObj->Release();

	if (Svc)
	  Svc->Release();

  CoUninitialize();
  } // end of CloseDB

// ------------------------ WCLCOL functions ----------------------------

/***********************************************************************/
/*  WCLCOL public constructor.                                         */
/***********************************************************************/
WCLCOL::WCLCOL(PCOLDEF cdp, PTDB tdbp, int n)
			: COLBLK(cdp, tdbp, n)
  {
	Tdbp = (PTDBWCL)tdbp;
	Flag = cdp->GetOffset();
	Res = 0;
  } // end of WMICOL constructor

/***********************************************************************/
/*  Read the next WCL elements.                                        */
/***********************************************************************/
void WCLCOL::ReadColumn(PGLOBAL g)
  {
  // Get the value of the Name property
	switch (Flag) {
		case 1:
	    Value->SetValue_psz(_com_util::ConvertBSTRToString(Tdbp->Propname));
			break;
		case 2:
	    Value->SetValue(Tdbp->Typ);
			break;
		case 3:
	    Value->SetValue_psz(GetTypeName(Tdbp->Typ));
			break;
		case 4:
		case 5:
	    Value->SetValue(Tdbp->Lng);
			break;
		case 6:
	    Value->SetValue(Tdbp->Prec);
			break;
		default:
			Value->Reset();
		} // endswitch Flag

  } // end of ReadColumn
